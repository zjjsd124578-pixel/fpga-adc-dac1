#include "dma_capture.h"
#include "scope_ctrl.h"
#include "scope_net.h"
#include "dds_ctrl.h"
#include "amplitude_loop.h"
#include "feedback_defaults.h"
#include "dpll_ctrl.h"
#include "sweep_ctrl.h"

#include "platform.h"
#include "platform_config.h"
#include "netif/xadapter.h"
#include "lwip/init.h"
#include "lwip/tcp.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xtime_l.h"

extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;
void tcp_fasttmr(void);
void tcp_slowtmr(void);

static struct netif board_netif;
/* platform_zynq.c 的定时器回调引用这个名字。 */
struct netif *echo_netif = &board_netif;
static u32 preview[SCOPE_PREVIEW_POINTS];

static int running;
static int frozen;
static u32 frozen_flags;
static u64 frozen_first;
static u32 frozen_count;
static u32 view_us = 10000U;
static u32 before_us = 2000U;
static u32 after_us = 2000U;
static u32 preview_stride = 159U;
static int window_pending;
static u32 feedback_samples[FEEDBACK_MAX_SAMPLES];
static u64 feedback_not_before;
static u32 feedback_generation;
static int feedback_report_pending;
static int feedback_last_state=-1;

static void feedback_track_output(void)
{
    u32 generation=DdsCtrl_GetGeneration();
    if(generation!=feedback_generation) {
        feedback_generation=generation;
        /* Timestamp the fence immediately after a commit, not at the next
         * 20 ms controller tick. Cover delayed completion IRQs, all queued
         * DMA descriptors, the AXIS FIFO, and two frames for DDS/ADC latency. */
        feedback_not_before=((u64)DmaCapture_GetCompletedFrames()+DMA_BD_COUNT+2U)*DMA_FRAME_SAMPLES+
                            FEEDBACK_AXIS_FIFO_SAMPLES;
        AmplitudeLoop_InvalidateMeasurement();
    }
}

static void service_feedback(void)
{
    u32 count, gain_q16, status_words[16];
    s32 bias_q16;
    u64 first;
    FeedbackMeasurement result;
    int ready=AmplitudeLoop_Ready(running);
    if (DpllCtrl_IsRequestedOrActive() || SweepCtrl_HardwarePiActive()) {
        AmplitudeLoop_Command("LOOP0");
        ready = 0;
    }
    feedback_track_output();
    if(ready && !FeedbackMeasure_WindowSamples(DdsCtrl_GetFtw(),&count)) {
        AmplitudeLoop_SetState(LOOP_BAD_FREQUENCY); goto report_status;
    }
    if(ready && DdsCtrl_IsIdle() && AmplitudeLoop_NeedsSeed()) {
        DdsCtrl_GetFeedback(&gain_q16,&bias_q16);
        if(!DdsCtrl_IsFeedbackMode()) gain_q16=0; /* Keep an open-loop OFFSET. */
        AmplitudeLoop_Seed(&gain_q16,&bias_q16);
        if(DdsCtrl_SetFeedback(gain_q16,bias_q16)!=DDS_CTRL_OK)
            AmplitudeLoop_SetState(LOOP_WAIT_DATA);
        else feedback_track_output();
        return;
    }
    if(!running || (!ready && !feedback_report_pending)) goto report_status;
    if(!FeedbackMeasure_WindowSamples(DdsCtrl_GetFtw(),&count)) {
        if(ready) {AmplitudeLoop_SetState(LOOP_BAD_FREQUENCY); goto report_status;}
        count=65536U; // Diagnostics can inspect a constant input without DDS.
    }
    if(!DdsCtrl_IsIdle() || !DmaCapture_CopyRaw(feedback_samples,count,&first) ||
       (ready && first<feedback_not_before)) {
        if(ready) AmplitudeLoop_SetState(LOOP_WAIT_DATA);
        goto report_status;
    }
    if(FeedbackMeasure_Calculate(feedback_samples,count,AmplitudeLoop_Calibration(),&result)) {
        if(ready) {
            if(AmplitudeLoop_Update(&result,&gain_q16,&bias_q16)) {
                if(DdsCtrl_SetFeedback(gain_q16,bias_q16)==DDS_CTRL_OK)
                    feedback_track_output();
                else AmplitudeLoop_SetState(LOOP_WAIT_DATA);
            }
        } else AmplitudeLoop_Diagnostic(&result);
    } else if(ready) AmplitudeLoop_SetState(LOOP_BAD_SIGNAL);
report_status:
    if(feedback_report_pending) {
        AmplitudeLoop_GetStatus(status_words);
        DdsCtrl_GetFeedback(&gain_q16,&bias_q16);
        status_words[12]=gain_q16; status_words[13]=(u32)bias_q16;
        SweepCtrl_OverrideLoopStatus(status_words);
        ScopeNet_ReportLoop(status_words);
        xil_printf("LOOP state=%u Vpp=%d/%d mV DC=%d/%d mV gain=%u biasQ16=%d\r\n",
            (unsigned int)status_words[1],(int)status_words[6],(int)status_words[4],
            (int)status_words[7],(int)status_words[5],(unsigned int)status_words[12],(int)status_words[13]);
        feedback_report_pending=0;
    }
}

static void service_pll(void)
{
    u32 words[DPLL_RESULT_WORDS], status;
    DpllCtrl_Poll(DdsCtrl_IsIdle());
    if (DpllCtrl_TakeReport(words, &status)) {
        ScopeNet_ReportPll(words, status);
        xil_printf("PLL status=0x%08x seq=%u gen=%u flags=0x%08x FTW=%u phaseError=%d lockCount=%u\r\n",
            (unsigned int)status, (unsigned int)words[DPLL_RESULT_SEQUENCE],
            (unsigned int)words[DPLL_RESULT_GENERATION],
            (unsigned int)words[DPLL_RESULT_FLAGS],
            (unsigned int)words[DPLL_RESULT_FTW],
            (int)(s32)words[DPLL_RESULT_PHASE_ERROR],
            (unsigned int)words[DPLL_RESULT_LOCK_COUNT]);
    }
}


static int arm_capture(void)
{
    u32 pre_samples;
    u32 post_samples;
    u32 half_view;

    if (ScopeNet_IsDumpBusy())
        return -1;
    ScopeCtrl_SetArm(0);
    ScopeCtrl_EnableAutoTrigger(0);
    half_view = view_us / 2U;
    pre_samples = (half_view + before_us) * 65U;
    post_samples = (view_us - half_view + after_us) * 65U;
    if (DmaCapture_Configure(pre_samples, post_samples) != 0)
        return -1;
    ScopeCtrl_SetPostSamples(post_samples);
    preview_stride = (u32)(((u64)view_us * 65U +
                            SCOPE_PREVIEW_POINTS - 1U) / SCOPE_PREVIEW_POINTS);
    if (DmaCapture_Start() != 0)
        return -1;
    ScopeCtrl_SetArm(1);
    running = 1;
    feedback_not_before=0;
    AmplitudeLoop_ResetMeasurement();
    frozen = 0;
    frozen_count = 0U;
    ScopeNet_ReportStatus(ScopeCtrl_ReadStatus(), 0U, 0U);
    xil_printf("Capture armed; rolling DDR ring active\r\n");
    return 0;
}

static void freeze_capture(void)
{
    u64 end;
    u32 post_count;
    u32 pre_count;
    frozen_flags = ScopeCtrl_ReadStatus();
    end = DmaCapture_GetTotalSamples();
    post_count = DmaCapture_GetPosttriggerSamples();
    if (end < post_count)
        post_count = (u32)end;
    pre_count = (end - post_count >= DmaCapture_GetPretriggerSamples()) ?
                DmaCapture_GetPretriggerSamples() : (u32)(end - post_count);
    frozen_count = pre_count + post_count;
    frozen_first = end - frozen_count;
    running = 0;
    frozen = 1;
    ScopeCtrl_SetArm(0);
    ScopeNet_ReportStatus(frozen_flags, end, frozen_count);
    if (ScopeNet_IsConnected() && frozen_count != 0U)
        ScopeNet_BeginDump(frozen_first, frozen_count, frozen_flags);
    xil_printf("DDR capture frozen: %u pre + %u post samples\r\n",
               (unsigned int)pre_count, (unsigned int)post_count);
}

int main(void)
{
    ip_addr_t ipaddr, netmask, gateway;
    unsigned char mac[] = { 0x00, 0x0a, 0x35, 0x00, 0x01, 0x02 };
    XTime now, next_preview, next_feedback;
    u64 first_sample;
    u32 commands, flags;

    init_platform();
    AmplitudeLoop_Init();
    DpllCtrl_Init((UINTPTR)DPLL_BASE_ADDRESS);
    SweepCtrl_Init();
    xil_printf("BOOT 1: PS platform initialized\r\n");
    xil_printf("ADC default scale: span=%d mV/4096 codes, reference code=%d at %d mV\r\n",
        (FEEDBACK_ADC_MV1-FEEDBACK_ADC_MV0)*4096/(FEEDBACK_ADC_CODE1-FEEDBACK_ADC_CODE0),
        FEEDBACK_ADC_CODE1,FEEDBACK_ADC_MV1);
    IP4_ADDR(&ipaddr, 192, 168, 1, 10);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 192, 168, 1, 1);
    lwip_init();
    if (!xemac_add(echo_netif, &ipaddr, &netmask, &gateway,
                   mac, PLATFORM_EMAC_BASEADDR)) {
        xil_printf("ERROR: add Ethernet interface failed\r\n");
        return -1;
    }
    xil_printf("BOOT 2: Ethernet interface initialized\r\n");
    netif_set_default(echo_netif);
    platform_enable_interrupts();
    netif_set_up(echo_netif);

    xil_printf("BOOT 3: before ADC control write at 0x%08x\r\n",
               (unsigned int)XPAR_SCOPE_AXI_CTRL_0_BASEADDR);
    ScopeCtrl_Init();
    xil_printf("BOOT 4: ADC control write completed\r\n");
    xil_printf("BOOT 5: before DDS ID read at 0x%08x\r\n",
               (unsigned int)(XPAR_DDS_AXI_CTRL_0_BASEADDR + DDS_REG_ID));
    if (DdsCtrl_Init((UINTPTR)XPAR_DDS_AXI_CTRL_0_BASEADDR) == DDS_CTRL_OK)
        xil_printf("DDS3/SWM1 firmware: PL VPP PI, corrected coarse/fine sweep 1k..1MHz; OFFSET open-loop; E0 close fix 20261015\r\n");
    else
        xil_printf("DDS init failed; load matching DDS3 feedback bitstream\r\n");
    ScopeCtrl_SetTrigger(0x0800U, 0, 0);
    ScopeCtrl_EnableAutoTrigger(0);
    xil_printf("BOOT 6: before DMA initialization\r\n");
    if (DmaCapture_Init() != 0) {
        xil_printf("ERROR: DMA initialization failed\r\n");
        return -1;
    }
    xil_printf("BOOT 7: DMA initialized; starting TCP server\r\n");
    if (ScopeNet_Start() != 0) {
        xil_printf("ERROR: TCP server startup failed\r\n");
        return -1;
    }
    if (arm_capture() != 0) {
        xil_printf("ERROR: capture arm failed\r\n");
        return -1;
    }

    xil_printf("Board IP 192.168.1.10, scope TCP port %u\r\n",
               SCOPE_NET_PORT);
    XTime_GetTime(&next_preview);
    next_feedback=next_preview;

    while (1) {
        if (TcpFastTmrFlag) {
            tcp_fasttmr();
            TcpFastTmrFlag = 0;
        }
        if (TcpSlowTmrFlag) {
            tcp_slowtmr();
            TcpSlowTmrFlag = 0;
        }
        xemacif_input(echo_netif);
        DdsCtrl_Poll();
        service_pll();
        XTime_GetTime(&now);
        SweepCtrl_Poll((u32)(((u64)now*1000U)/COUNTS_PER_SECOND));
        feedback_track_output();
        if(DdsCtrl_IsIdle()) {
            s32 manual_bias; u32 manual_gain;
            if(AmplitudeLoop_TakeOpenLoopOffset(&manual_bias)) {
                if(DdsCtrl_SetOpenLoopOffset(manual_bias)==DDS_CTRL_OK) {
                    feedback_track_output();
                    s32 applied_bias;
                    DdsCtrl_GetFeedback(&manual_gain,&applied_bias);
                    AmplitudeLoop_SyncOutput(manual_gain,applied_bias);
                } else xil_printf("OFFSET update rejected\r\n");
            }
            if(DdsCtrl_IsIdle() && AmplitudeLoop_TakeManualBias(&manual_bias)) {
                s32 previous_bias;
                DdsCtrl_GetFeedback(&manual_gain,&previous_bias);
                if(DdsCtrl_SetFeedback(manual_gain,manual_bias)!=DDS_CTRL_OK)
                    xil_printf("BIAS rejected: reduce A first to reserve DAC headroom\r\n");
                else feedback_track_output();
            }
        }
        commands = ScopeNet_TakeCommands();
        if ((commands & SCOPE_CMD_WINDOW) != 0U) {
            ScopeNet_GetWindow(&view_us, &before_us, &after_us);
            window_pending = 1;
        }
        if (window_pending && !ScopeNet_IsDumpBusy()) {
            if (arm_capture() == 0)
                window_pending = 0;
        }
        if ((commands & SCOPE_CMD_ARM) != 0U)
            arm_capture();
        if (running && (commands & SCOPE_CMD_TRIGGER) != 0U)
            ScopeCtrl_EnableAutoTrigger(1);
        if (running && (commands & SCOPE_CMD_PAUSE) != 0U)
            ScopeCtrl_ManualTrigger();
        if (frozen && (commands & SCOPE_CMD_DUMP) != 0U &&
            !ScopeNet_IsDumpBusy() && frozen_count != 0U)
            ScopeNet_BeginDump(frozen_first, frozen_count, frozen_flags);

        if (running) {
            flags = ScopeCtrl_ReadStatus();
            if (DmaCapture_HasError() ||
                (flags & SCOPE_STATUS_OVERFLOW) != 0U) {
                xil_printf("ERROR: capture overflow: dma_bds=%u pl_frames=%u\r\n",
                           (unsigned int)DmaCapture_GetCompletedFrames(),
                           (unsigned int)ScopeCtrl_ReadCompletedFrames());
                running = 0;
                ScopeCtrl_SetArm(0);
                ScopeNet_ReportStatus(flags | SCOPE_STATUS_OVERFLOW,
                                      DmaCapture_GetTotalSamples(), 0U);
            } else if (DmaCapture_IsDone()) {
                freeze_capture();
            }
        }

        feedback_report_pending |= AmplitudeLoop_TakeReport();
        XTime_GetTime(&now);
        if(now>=next_feedback) {
            next_feedback=now+COUNTS_PER_SECOND/50U;
            service_feedback();
            if(AmplitudeLoop_State()!=feedback_last_state) {
                feedback_last_state=AmplitudeLoop_State();
                xil_printf("Feedback state=%d (use LOOPSTATUS for details)\r\n",feedback_last_state);
            }
        }
        if (now >= next_preview) {
            next_preview = now + COUNTS_PER_SECOND / 30U;
            if (running && ScopeNet_IsConnected() &&
                !ScopeNet_IsDumpBusy() &&
                DmaCapture_CopyPreview(preview, SCOPE_PREVIEW_POINTS,
                                       preview_stride, &first_sample)) {
                ScopeNet_QueuePreview(first_sample, preview,
                                      SCOPE_PREVIEW_POINTS,
                                      preview_stride,
                                      ScopeCtrl_ReadStatus());
            }
        }
        ScopeNet_Poll();
    }
}
