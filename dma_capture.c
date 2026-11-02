#include "dma_capture.h"
#include "scope_ctrl.h"

#include "xparameters.h"
#include "xaxidma.h"
#include "xscugic.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "xstatus.h"
#include "sleep.h"
#include <string.h>

/*
 * 下面两个名称由 Vitis 根据 XSA 生成。
 * 如果编译提示未定义，请在 xparameters.h 中搜索 AXIDMA_0、S2MM_INTROUT，
 * 然后替换为你的工程实际宏名。
 */
#define DMA_DEVICE_ID              XPAR_AXIDMA_0_DEVICE_ID
#define DMA_INTR_ID                XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR

/* PS GIC 设备号，Zynq-7000 standalone BSP 通常生成这个宏。 */
#define INTC_DEVICE_ID             XPAR_SCUGIC_SINGLE_DEVICE_ID

/* 描述符在程序区；波形记录区由 linker script 在 64 MiB 以上保留。 */
#define DMA_BD_MEMORY_SIZE         0x00010000U
#define DMA_MAX_BUFFER_SIZE        (DMA_MAX_RECORD_BLOCK_COUNT * DMA_BYTES_PER_FRAME)

static u8 dma_bd_memory[DMA_BD_MEMORY_SIZE]
    __attribute__((aligned(XAXIDMA_BD_MINIMUM_ALIGNMENT)));
#define DMA_BD_BASE_ADDR           ((UINTPTR)&dma_bd_memory[0])
#define DMA_BUFFER_BASE_ADDR       ((UINTPTR)DMA_RECORD_BASE_ADDR)

/*
 * capture_done 后，读取 PL 的已发送帧数，等待相同数量的 DMA 描述符完成。
 * 不能仅凭完成计数暂时不变判断最后一个 DDR 写入已经结束。
 */
#define DMA_DRAIN_POLL_US          50U
#define DMA_DRAIN_TIMEOUT_US       100000U

/* 每 8 帧触发一次普通中断；不足 8 帧时由延迟中断保证最终仍会响应。 */
#define DMA_IRQ_COALESCE_COUNT     8U
#define DMA_IRQ_DELAY_COUNT        32U

/* DMA 复位等待上限。 */
#define DMA_RESET_TIMEOUT          1000000U

static XAxiDma axi_dma;
static XAxiDma_BdRing *s2mm_ring;

static volatile int dma_done;
static volatile int dma_error;
static volatile int stop_recycle;
static volatile u32 last_completed_addr;
static volatile u32 last_completed_length;
static volatile u32 completed_bd_count;

/* 下一次提交描述符时使用的环形缓冲块编号。 */
static u32 next_buffer_index;
static u32 record_block_count = DMA_MIN_RECORD_BLOCK_COUNT;
static u32 active_bd_count = DMA_BD_COUNT / 2U;
static u32 pretrigger_samples;
static u32 posttrigger_samples;

static int DmaCapture_SetupInterrupt(void);
static int DmaCapture_ResetAndCreateRing(void);
static int DmaCapture_SubmitBds(int bd_count);
static void DmaCapture_S2mmIsr(void *callback_ref);

/* 初始化 DMA 驱动和 PS 中断，只需在程序启动时调用一次。 */
int DmaCapture_Init(void)
{
    XAxiDma_Config *config;
    int status;

    dma_done = 0;
    dma_error = 0;
    stop_recycle = 1;
    last_completed_addr = (u32)DMA_BUFFER_BASE_ADDR;
    last_completed_length = 0U;
    completed_bd_count = 0U;
    next_buffer_index = 0U;

    if (((u64)DMA_RECORD_BASE_ADDR + (u64)DMA_MAX_BUFFER_SIZE) >
        ((u64)XPAR_PS7_DDR_0_S_AXI_HIGHADDR + 1ULL)) {
        xil_printf("ERROR: DDR record region exceeds physical RAM\r\n");
        return XST_FAILURE;
    }

    config = XAxiDma_LookupConfig(DMA_DEVICE_ID);
    if (config == NULL) {
        xil_printf("ERROR: cannot find AXI DMA config\r\n");
        return XST_FAILURE;
    }

    status = XAxiDma_CfgInitialize(&axi_dma, config);
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: AXI DMA init failed\r\n");
        return XST_FAILURE;
    }

    if (!XAxiDma_HasSg(&axi_dma)) {
        xil_printf("ERROR: AXI DMA is not Scatter Gather mode\r\n");
        return XST_FAILURE;
    }

    status = DmaCapture_SetupInterrupt();
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: DMA interrupt setup failed\r\n");
        return XST_FAILURE;
    }

    status = DmaCapture_ResetAndCreateRing();
    if (status != XST_SUCCESS)
        return XST_FAILURE;

    xil_printf("AXI DMA S2MM init OK\r\n");
    return XST_SUCCESS;
}

int DmaCapture_Configure(u32 pre_samples, u32 post_samples)
{
    u64 total = (u64)pre_samples + post_samples;
    u32 blocks;

    if (pre_samples == 0U || post_samples == 0U ||
        total > DMA_MAX_CAPTURE_SAMPLES)
        return XST_FAILURE;

    stop_recycle = 1;

    /* 留两个完整帧，避免末帧和正在回收的描述符覆盖所需窗口。 */
    blocks = (u32)((total + DMA_FRAME_SAMPLES - 1U) / DMA_FRAME_SAMPLES) + 2U;
    if (blocks < DMA_MIN_RECORD_BLOCK_COUNT)
        blocks = DMA_MIN_RECORD_BLOCK_COUNT;
    if (blocks > DMA_MAX_RECORD_BLOCK_COUNT)
        return XST_FAILURE;

    record_block_count = blocks;
    active_bd_count = (blocks / 2U < DMA_BD_COUNT) ?
                      blocks / 2U : DMA_BD_COUNT;
    pretrigger_samples = pre_samples;
    posttrigger_samples = post_samples;
    xil_printf("DDR ring: %u blocks, %u bytes, pre=%u post=%u\r\n",
               (unsigned int)blocks,
               (unsigned int)(blocks * DMA_BYTES_PER_FRAME),
               (unsigned int)pre_samples,
               (unsigned int)post_samples);
    return XST_SUCCESS;
}

/* 开始一次新的循环采集。main.c 必须在此函数成功后才能设置 arm=1。 */
int DmaCapture_Start(void)
{
    int status;

    if (pretrigger_samples == 0U || posttrigger_samples == 0U)
        return XST_FAILURE;

    dma_done = 0;
    dma_error = 0;
    stop_recycle = 1;
    last_completed_addr = (u32)DMA_BUFFER_BASE_ADDR;
    last_completed_length = 0U;
    completed_bd_count = 0U;
    next_buffer_index = 0U;

    /* 重复采集时，复位 DMA 并重新建立全部 SG 描述符状态。 */
    status = DmaCapture_ResetAndCreateRing();
    if (status != XST_SUCCESS)
        return XST_FAILURE;

    stop_recycle = 0;

    /* 记录区没有 CPU 写入；读取完成的块时再按需 invalidate。 */
    /* 排队长度小于实际环长度，短时间窗也不会同时提交重复的 DDR 地址。 */
    status = DmaCapture_SubmitBds((int)active_bd_count);
    if (status != XST_SUCCESS) {
        dma_error = 1;
        return XST_FAILURE;
    }

    status = XAxiDma_BdRingStart(s2mm_ring);
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: S2MM ring start failed\r\n");
        dma_error = 1;
        return XST_FAILURE;
    }

    xil_printf("DMA capture started\r\n");
    return XST_SUCCESS;
}

/*
 * packer 的 capture_done 表示最后一点已被 FIFO 接收，并不表示已进入 DDR。
 * 等待 DMA 完成全部 TLAST 帧的 BD 后才允许 CPU 读取缓冲区。
 */
int DmaCapture_IsDone(void)
{
    u32 status;
    u32 expected_frames;
    u32 elapsed_time;

    if (dma_error)
        return 0;

    if (dma_done)
        return 1;

    status = ScopeCtrl_ReadStatus();
    if ((status & SCOPE_STATUS_DONE) == 0U)
        return 0;

    /*
     * PL 帧数在 capture_done 可见时已锁存。每个 TLAST 帧对应一个
     * S2MM BD，最后的部分帧也必须等到对应 BD 从 DMA 回收。
     */
    expected_frames = ScopeCtrl_ReadCompletedFrames();
    if (expected_frames == 0U) {
        xil_printf("ERROR: PL reported zero completed frames\r\n");
        dma_error = 1;
        return 0;
    }

    elapsed_time = 0U;

    while (completed_bd_count < expected_frames &&
           elapsed_time < DMA_DRAIN_TIMEOUT_US) {
        if (dma_error)
            return 0;

        usleep(DMA_DRAIN_POLL_US);
        elapsed_time += DMA_DRAIN_POLL_US;
    }

    if (completed_bd_count < expected_frames) {
        xil_printf("ERROR: DMA drain timeout (%u/%u frames)\r\n",
                   (unsigned int)completed_bd_count,
                   (unsigned int)expected_frames);
        dma_error = 1;
        return 0;
    }

    stop_recycle = 1;

    /* 排空后不再需要 DMA 中断；下次 Start 会复位并重建描述符环。 */
    XAxiDma_BdRingIntDisable(s2mm_ring, XAXIDMA_IRQ_ALL_MASK);

    dma_done = 1;
    return 1;
}

int DmaCapture_HasError(void)
{
    return dma_error ? 1 : 0;
}

u32 DmaCapture_GetBufferAddr(void)
{
    return (u32)DMA_BUFFER_BASE_ADDR;
}

u32 DmaCapture_GetBufferSize(void)
{
    return record_block_count * DMA_BYTES_PER_FRAME;
}

u32 DmaCapture_GetCompletedFrames(void)
{
    return completed_bd_count;
}

u64 DmaCapture_GetTotalSamples(void)
{
    u32 frames = completed_bd_count;
    if (frames == 0U)
        return 0ULL;
    return ((u64)(frames - 1U) * DMA_FRAME_SAMPLES) +
           ((u64)last_completed_length / DMA_BYTES_PER_SAMPLE);
}

u32 DmaCapture_GetPretriggerSamples(void)
{
    return pretrigger_samples;
}

u32 DmaCapture_GetPosttriggerSamples(void)
{
    return posttrigger_samples;
}

u32 DmaCapture_GetSampleAddr(u64 absolute_sample)
{
    u32 index = (u32)(absolute_sample % (record_block_count * DMA_FRAME_SAMPLES));
    return DMA_RECORD_BASE_ADDR + (index * DMA_BYTES_PER_SAMPLE);
}

void DmaCapture_Invalidate(u32 addr, u32 bytes)
{
    Xil_DCacheInvalidateRange((UINTPTR)addr, bytes);
}

int DmaCapture_CopyPreview(u32 *destination, u32 point_count,
                           u32 stride, u64 *first_sample)
{
    u32 frames = completed_bd_count;
    u64 end_sample = (u64)frames * DMA_FRAME_SAMPLES;
    u64 window_samples = (u64)point_count * stride;
    u64 start_sample;
    u32 start_index;
    u32 first_part;
    u32 i;
    u32 index;
    u32 record_samples = record_block_count * DMA_FRAME_SAMPLES;
    volatile u32 *record = (volatile u32 *)(UINTPTR)DMA_RECORD_BASE_ADDR;

    if (destination == NULL || first_sample == NULL || point_count == 0U ||
        stride == 0U || window_samples > record_samples ||
        end_sample < window_samples)
        return 0;

    start_sample = end_sample - window_samples;
    start_index = (u32)(start_sample % record_samples);
    if (stride <= 64U) {
        first_part = record_samples - start_index;
        if ((u64)first_part > window_samples)
            first_part = (u32)window_samples;
        DmaCapture_Invalidate(DMA_RECORD_BASE_ADDR + start_index * 4U,
                              first_part * 4U);
        if ((u64)first_part < window_samples)
            DmaCapture_Invalidate(DMA_RECORD_BASE_ADDR,
                                  (u32)(window_samples - first_part) * 4U);
    }

    index = start_index;
    for (i = 0U; i < point_count; ++i) {
        if (stride > 64U)
            DmaCapture_Invalidate(DMA_RECORD_BASE_ADDR + index * 4U, 4U);
        destination[i] = record[index];
        index += stride;
        if (index >= record_samples)
            index -= record_samples;
    }
    *first_sample = start_sample;
    return 1;
}

int DmaCapture_CopyRaw(u32 *destination, u32 count, u64 *first_sample)
{
    u32 frames=completed_bd_count;
    u64 end=(u64)frames*DMA_FRAME_SAMPLES;
    u64 first, writer_end;
    u32 ring=record_block_count*DMA_FRAME_SAMPLES, index, part;
    if (!destination || !first_sample || !count || dma_error || dma_done ||
        count >= ring || end < count ||
        count + active_bd_count*DMA_FRAME_SAMPLES >= ring) return 0;
    first=end-count; index=(u32)(first%ring);
    part=ring-index; if(part>count) part=count;
    DmaCapture_Invalidate(DMA_RECORD_BASE_ADDR+index*4U, part*4U);
    memcpy(destination,(const void *)(UINTPTR)(DMA_RECORD_BASE_ADDR+index*4U),part*4U);
    if(part<count) {
        DmaCapture_Invalidate(DMA_RECORD_BASE_ADDR,(count-part)*4U);
        memcpy(destination+part,(const void *)(UINTPTR)DMA_RECORD_BASE_ADDR,(count-part)*4U);
    }
    /* Include all submitted descriptors, not only the interrupt-visible count. */
    writer_end=((u64)completed_bd_count+active_bd_count)*DMA_FRAME_SAMPLES;
    if(dma_error || dma_done || writer_end-first >= ring) return 0;
    *first_sample=first;
    return 1;
}

u32 DmaCapture_GetLastCompletedAddr(void)
{
    return last_completed_addr;
}

u32 DmaCapture_GetLastCompletedLength(void)
{
    return last_completed_length;
}

u32 DmaCapture_GetValidBlockCount(void)
{
    if (completed_bd_count >= record_block_count)
        return record_block_count;

    return completed_bd_count;
}

u32 DmaCapture_GetOldestBufferAddr(void)
{
    u32 last_index;
    u32 oldest_index;

    if (completed_bd_count < record_block_count)
        return (u32)DMA_BUFFER_BASE_ADDR;

    last_index = (last_completed_addr - (u32)DMA_BUFFER_BASE_ADDR) /
                 DMA_BYTES_PER_FRAME;
    oldest_index = last_index + 1U;

    if (oldest_index >= record_block_count)
        oldest_index = 0U;

    return (u32)DMA_BUFFER_BASE_ADDR +
           (oldest_index * DMA_BYTES_PER_FRAME);
}

/* 复位 DMA，然后在 DDR 中重新创建空的 S2MM 描述符环。 */
static int DmaCapture_ResetAndCreateRing(void)
{
    XAxiDma_Bd bd_template;
    u32 timeout;
    int status;

    XAxiDma_Reset(&axi_dma);
    timeout = DMA_RESET_TIMEOUT;

    while (!XAxiDma_ResetIsDone(&axi_dma)) {
        if (timeout == 0U) {
            xil_printf("ERROR: AXI DMA reset timeout\r\n");
            return XST_FAILURE;
        }
        timeout--;
    }

    s2mm_ring = XAxiDma_GetRxRing(&axi_dma);

    status = XAxiDma_BdRingCreate(s2mm_ring,
                                  (UINTPTR)DMA_BD_BASE_ADDR,
                                  (UINTPTR)DMA_BD_BASE_ADDR,
                                  XAXIDMA_BD_MINIMUM_ALIGNMENT,
                                  (int)DMA_BD_COUNT);
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: create S2MM BD ring failed\r\n");
        return XST_FAILURE;
    }

    XAxiDma_BdClear(&bd_template);
    status = XAxiDma_BdRingClone(s2mm_ring, &bd_template);
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: clone S2MM BD failed\r\n");
        return XST_FAILURE;
    }

    status = XAxiDma_BdRingSetCoalesce(s2mm_ring,
                                       DMA_IRQ_COALESCE_COUNT,
                                       DMA_IRQ_DELAY_COUNT);
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: set S2MM coalesce failed\r\n");
        return XST_FAILURE;
    }

    XAxiDma_BdRingIntEnable(s2mm_ring,
                            XAXIDMA_IRQ_IOC_MASK |
                            XAXIDMA_IRQ_DELAY_MASK |
                            XAXIDMA_IRQ_ERROR_MASK);

    return XST_SUCCESS;
}

/* 从软件空闲队列取出描述符，填写 DDR 地址和长度，再交给 DMA。 */
static int DmaCapture_SubmitBds(int bd_count)
{
    XAxiDma_Bd *first_bd;
    XAxiDma_Bd *current_bd;
    UINTPTR buffer_addr;
    int status;
    int i;

    if (DMA_BYTES_PER_FRAME > s2mm_ring->MaxTransferLen) {
        xil_printf("ERROR: DMA BD length %u exceeds hardware limit %u\r\n",
                   (unsigned int)DMA_BYTES_PER_FRAME,
                   (unsigned int)s2mm_ring->MaxTransferLen);
        return XST_FAILURE;
    }

    status = XAxiDma_BdRingAlloc(s2mm_ring, bd_count, &first_bd);
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: allocate %d S2MM BDs failed\r\n", bd_count);
        return XST_FAILURE;
    }

    current_bd = first_bd;

    for (i = 0; i < bd_count; ++i) {
        buffer_addr = (UINTPTR)DMA_BUFFER_BASE_ADDR +
                      ((UINTPTR)next_buffer_index * DMA_BYTES_PER_FRAME);

        status = XAxiDma_BdSetBufAddr(current_bd, buffer_addr);
        if (status != XST_SUCCESS)
            return XST_FAILURE;

        status = XAxiDma_BdSetLength(current_bd,
                                     DMA_BYTES_PER_FRAME,
                                     s2mm_ring->MaxTransferLen);
        if (status != XST_SUCCESS) {
            xil_printf("ERROR: invalid S2MM BD length\r\n");
            return XST_FAILURE;
        }

        /* S2MM 的帧结束由 adc_axis_packer 的 TLAST 决定。 */
        XAxiDma_BdSetCtrl(current_bd, 0U);
        XAxiDma_BdSetId(current_bd, buffer_addr);

        next_buffer_index++;
        if (next_buffer_index >= record_block_count)
            next_buffer_index = 0U;

        current_bd = (XAxiDma_Bd *)XAxiDma_BdRingNext(s2mm_ring, current_bd);
    }

    status = XAxiDma_BdRingToHw(s2mm_ring, bd_count, first_bd);
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: submit S2MM BDs failed\r\n");
        return XST_FAILURE;
    }

    return XST_SUCCESS;
}

/* 初始化 Zynq PS GIC，并把 DMA S2MM 中断连接到 ISR。 */
static int DmaCapture_SetupInterrupt(void)
{
    /* lwIP platform_zynq.c 已初始化 GIC；与 GEM 共用同一个低层表。 */
    XScuGic_SetPriTrigTypeByDistAddr(XPAR_SCUGIC_0_DIST_BASEADDR,
                                     DMA_INTR_ID, 0xA0U, 0x3U);
    XScuGic_RegisterHandler(XPAR_SCUGIC_0_CPU_BASEADDR,
                            DMA_INTR_ID,
                            (Xil_ExceptionHandler)DmaCapture_S2mmIsr,
                            (void *)&axi_dma);
    XScuGic_EnableIntr(XPAR_SCUGIC_0_DIST_BASEADDR, DMA_INTR_ID);

    return XST_SUCCESS;
}

/* DMA S2MM 中断服务函数：回收已完成描述符，并在采集中继续循环提交。 */
static void DmaCapture_S2mmIsr(void *callback_ref)
{
    XAxiDma_Bd *completed_bd;
    XAxiDma_Bd *current_bd;
    XAxiDma *dma_ptr;
    u32 irq_status;
    u32 actual_length;
    int bd_count;
    int status;
    int i;

    dma_ptr = (XAxiDma *)callback_ref;
    s2mm_ring = XAxiDma_GetRxRing(dma_ptr);

    irq_status = XAxiDma_BdRingGetIrq(s2mm_ring);
    XAxiDma_BdRingAckIrq(s2mm_ring, irq_status);

    if ((irq_status & XAXIDMA_IRQ_ALL_MASK) == 0U)
        return;

    if ((irq_status & XAXIDMA_IRQ_ERROR_MASK) != 0U) {
        dma_error = 1;
        stop_recycle = 1;
        XAxiDma_Reset(dma_ptr);
        return;
    }

    if ((irq_status & (XAXIDMA_IRQ_IOC_MASK |
                       XAXIDMA_IRQ_DELAY_MASK)) == 0U)
        return;

    bd_count = XAxiDma_BdRingFromHw(s2mm_ring,
                                     XAXIDMA_ALL_BDS,
                                     &completed_bd);
    if (bd_count <= 0)
        return;

    /* 记录最近完成的 DDR 缓冲块地址。 */
    current_bd = completed_bd;
    for (i = 0; i < bd_count; ++i) {
        last_completed_addr = (u32)XAxiDma_BdGetId(current_bd);
        actual_length = XAxiDma_BdGetActualLength(current_bd,
                                                   s2mm_ring->MaxTransferLen);
        last_completed_length = actual_length;
        completed_bd_count++;
        current_bd = (XAxiDma_Bd *)XAxiDma_BdRingNext(s2mm_ring, current_bd);
    }

    status = XAxiDma_BdRingFree(s2mm_ring, bd_count, completed_bd);
    if (status != XST_SUCCESS) {
        dma_error = 1;
        stop_recycle = 1;
        return;
    }

    /* 触发采集尚未结束时，把同样数量的描述符重新放回硬件队列。 */
    if (!stop_recycle) {
        status = DmaCapture_SubmitBds(bd_count);
        if (status != XST_SUCCESS) {
            dma_error = 1;
            stop_recycle = 1;
        }
    }
}
