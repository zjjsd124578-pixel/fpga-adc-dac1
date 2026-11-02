#ifndef SCOPE_CTRL_H
#define SCOPE_CTRL_H

#include "xil_types.h"

/* scope_axi_ctrl.v 中定义的寄存器偏移地址 */
#define SCOPE_REG_CONTROL         0x00U
#define SCOPE_REG_TRIGGER_LEVEL   0x04U
#define SCOPE_REG_STATUS          0x08U
#define SCOPE_REG_COMPLETED_FRAMES 0x0CU
#define SCOPE_REG_POST_SAMPLES      0x10U

/* CONTROL 寄存器的 bit 定义 */
#define SCOPE_CTRL_ARM             (1U << 0)
#define SCOPE_CTRL_MANUAL_TRIGGER  (1U << 1)
#define SCOPE_CTRL_TRIGGER_ENABLE  (1U << 2)
#define SCOPE_CTRL_TRIGGER_CHANNEL (1U << 3)  /* 0：A，1：B */
#define SCOPE_CTRL_TRIGGER_EDGE    (1U << 4)  /* 0：上升，1：下降 */

/* STATUS 寄存器的 bit 定义 */
#define SCOPE_STATUS_ACTIVE        (1U << 0)
#define SCOPE_STATUS_TRIGGERED     (1U << 1)
#define SCOPE_STATUS_DONE          (1U << 2)
#define SCOPE_STATUS_OVERFLOW      (1U << 3)

/* 后续由 scope_ctrl.c 实现的函数 */
void ScopeCtrl_Init(void);
void ScopeCtrl_SetTrigger(u16 level, int channel_b, int falling_edge);
void ScopeCtrl_SetPostSamples(u32 count);
void ScopeCtrl_EnableAutoTrigger(int enable);
void ScopeCtrl_SetArm(int arm);
void ScopeCtrl_ManualTrigger(void);
u32  ScopeCtrl_ReadStatus(void);
u32  ScopeCtrl_ReadCompletedFrames(void);

#endif
