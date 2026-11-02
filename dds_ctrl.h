#ifndef DDS_CTRL_H
#define DDS_CTRL_H
#include "xil_types.h"
#include "dds_command.h"

#define DDS_REG_STATUS  0x00U
#define DDS_REG_COMMIT  0x04U
#define DDS_REG_CONTROL 0x08U
#define DDS_REG_WAVE    0x0CU
#define DDS_REG_FTW     0x10U
#define DDS_REG_PHASE   0x14U
#define DDS_REG_START   0x18U
#define DDS_REG_STOP    0x1CU
#define DDS_REG_STEP    0x20U
#define DDS_REG_ID      0x24U
#define DDS_REG_AMPLITUDE 0x28U // Q16 gain: 0..65536
#define DDS_REG_OFFSET 0x2CU // signed DAC code offset, Q16
#define DDS_STATUS_BUSY 1U
#define DDS_DEVICE_ID   0x44445333U
#define DDS_QUEUE_DEPTH 16U

enum { DDS_CTRL_OK = 0, DDS_CTRL_FULL = -1, DDS_CTRL_INVALID = -2,
       DDS_CTRL_NOT_READY = -3 };
/* Use the actual address from Vivado Address Editor / generated xparameters.h.
 * No hardcoded address is assumed. Hardware defaults to disabled output.
 */
int DdsCtrl_Init(UINTPTR base_address);
int DdsCtrl_QueueCommand(const DdsCommand *command);
/* Nonblocking; call from the existing main loop, never wait in lwIP callbacks. */
void DdsCtrl_Poll(void);
/* True only after pending queued settings have been applied in the DDS domain. */
int DdsCtrl_IsIdle(void);
/* Atomic latest feedback value; accepted only while the manual queue is idle.
 * Selects ordinary sine mode, preserving frequency and output enable.
 * It never reopens E0; enabling output requires an explicit E1. */
int DdsCtrl_SetFeedback(u32 gain_q16, s32 offset_q16);
/* Open-loop bias: preserves waveform/frequency/enable and reduces gain only
 * if needed to reserve headroom. No ADC measurement or PI is used. */
int DdsCtrl_SetOpenLoopOffset(s32 offset_q16);
void DdsCtrl_GetFeedback(u32 *gain_q16, s32 *offset_q16);
u32 DdsCtrl_GetFtw(void);
u32 DdsCtrl_GetGeneration(void);
int DdsCtrl_IsFeedbackMode(void);
#endif
