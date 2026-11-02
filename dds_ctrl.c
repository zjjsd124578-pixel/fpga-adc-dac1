#include "dds_ctrl.h"
#include "xil_io.h"

static UINTPTR dds_base;
static int initialized;
static s32 offset_value;
static u32 generation;
static void commit_settings(u32 restart);
static DdsCommand queue[DDS_QUEUE_DEPTH];
static unsigned int queue_read, queue_write, queue_count;
static u32 control_value, wave_value, ftw_value, phase_value;
static u32 start_value, stop_value, step_value, amplitude_value;

static int valid_command(const DdsCommand *command)
{
    if (command == NULL) return 0;
    switch (command->code) {
    case 'A': return command->value <= 1000U;
    case 'W': return command->value >= 1U && command->value <= 5U;
    case 'E': case 'S': return command->value <= 1U;
    case 'F': case 'P': case 'B': case 'C': case 'D': return 1;
    default: return 0;
    }
}
int DdsCtrl_Init(UINTPTR base_address)
{
    initialized = 0;
    if (Xil_In32(base_address + DDS_REG_ID) != DDS_DEVICE_ID)
        return DDS_CTRL_NOT_READY;
    dds_base = base_address;
    queue_read = 0U; queue_write = 0U; queue_count = 0U;
    control_value = 0U; wave_value = 1U; ftw_value = 100000U;
    phase_value = 0U; start_value = 0U; stop_value = 0U; step_value = 0U;
    amplitude_value = 65536U; offset_value = 0; generation = 0;
    initialized = 1;
    /* Queue a disabling configuration, also covering a PS-only restart. */
    queue[0].code = 'E'; queue[0].value = 0U;
    queue_write = 1U; queue_count = 1U;
    return DDS_CTRL_OK;
}
int DdsCtrl_QueueCommand(const DdsCommand *command)
{
    if (!initialized) return DDS_CTRL_NOT_READY;
    if (!valid_command(command)) return DDS_CTRL_INVALID;
    /* A close request supersedes queued settings, especially an older E1.
     * This changes RAM only; all hardware writes stay in the main loop. */
    if (command->code == 'E' && command->value == 0U)
        queue_read = queue_write = queue_count = 0U;
    if (queue_count == DDS_QUEUE_DEPTH) return DDS_CTRL_FULL;
    queue[queue_write] = *command;
    queue_write = (queue_write + 1U) % DDS_QUEUE_DEPTH;
    ++queue_count;
    return DDS_CTRL_OK;
}
void DdsCtrl_Poll(void)
{
    DdsCommand command;
    if (!initialized || queue_count == 0U ||
        (Xil_In32(dds_base + DDS_REG_STATUS) & DDS_STATUS_BUSY) != 0U)
        return;
    command = queue[queue_read];
    queue_read = (queue_read + 1U) % DDS_QUEUE_DEPTH;
    --queue_count;
    switch (command.code) {
    case 'A': amplitude_value = (command.value * 65536U + 500U) / 1000U; break;
    case 'W': wave_value = command.value; break;
    case 'F': ftw_value = command.value; break;
    case 'P': phase_value = command.value; break;
    case 'E': control_value = (control_value & ~1U) | command.value; break;
    case 'S': control_value = (control_value & ~2U) | (command.value << 1); break;
    case 'B': start_value = command.value; break;
    case 'C': stop_value = command.value; break;
    case 'D': step_value = command.value; break;
    default: return;
    }
    commit_settings(1U); // Preserve explicit/manual restart behavior.
}
static void commit_settings(u32 restart)
{
    Xil_Out32(dds_base + DDS_REG_CONTROL, control_value);
    Xil_Out32(dds_base + DDS_REG_WAVE, wave_value);
    Xil_Out32(dds_base + DDS_REG_FTW, ftw_value);
    Xil_Out32(dds_base + DDS_REG_PHASE, phase_value);
    Xil_Out32(dds_base + DDS_REG_START, start_value);
    Xil_Out32(dds_base + DDS_REG_STOP, stop_value);
    Xil_Out32(dds_base + DDS_REG_STEP, step_value);
    Xil_Out32(dds_base + DDS_REG_AMPLITUDE, amplitude_value);
    Xil_Out32(dds_base + DDS_REG_OFFSET, (u32)offset_value);
    Xil_Out32(dds_base + DDS_REG_COMMIT, restart ? 3U : 1U);
    ++generation;
}
int DdsCtrl_SetFeedback(u32 gain_q16, s32 bias_q16)
{
    s32 center;
    if (!DdsCtrl_IsIdle()) return DDS_CTRL_NOT_READY;
    if (gain_q16 > 65536U || bias_q16 < -8388608 || bias_q16 > 8323072)
        return DDS_CTRL_INVALID;
    /* Reserve half a DAC code at either end for rounding. */
    center = 8388608 + bias_q16;
    if ((s32)(128U * gain_q16) > center ||
        (s32)(127U * gain_q16) > 16711680 - center)
        return DDS_CTRL_INVALID;
    /* Updating/holding feedback gain must never reopen a closed output.
     * Only an explicit E1 command may change a disabled output to enabled. */
    control_value &= 1U; wave_value = 1U;
    amplitude_value = gain_q16; offset_value = bias_q16;
    commit_settings(0U);
    return DDS_CTRL_OK;
}
int DdsCtrl_SetOpenLoopOffset(s32 bias_q16)
{
    u32 ceiling, other;
    s32 center;
    if (!DdsCtrl_IsIdle()) return DDS_CTRL_NOT_READY;
    if (bias_q16 < -8323072 || bias_q16 > 8257536) return DDS_CTRL_INVALID;
    center = 8388608 + bias_q16;
    ceiling = (u32)center / 128U;
    other = (u32)(16711680 - center) / 127U;
    if (other < ceiling) ceiling = other;
    if (amplitude_value > ceiling) amplitude_value = ceiling;
    offset_value = bias_q16;
    commit_settings(0U);
    return DDS_CTRL_OK;
}
void DdsCtrl_GetFeedback(u32 *gain_q16, s32 *bias_q16)
{
    *gain_q16 = amplitude_value; *bias_q16 = offset_value;
}
u32 DdsCtrl_GetFtw(void) { return ftw_value; }
u32 DdsCtrl_GetGeneration(void) { return generation; }
int DdsCtrl_IsFeedbackMode(void) { return initialized && control_value == 1U && wave_value == 1U; }

int DdsCtrl_IsIdle(void)
{
    return initialized && queue_count == 0U &&
           (Xil_In32(dds_base + DDS_REG_STATUS) & DDS_STATUS_BUSY) == 0U;
}
