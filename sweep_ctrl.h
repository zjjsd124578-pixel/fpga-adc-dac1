#ifndef SWEEP_CTRL_H
#define SWEEP_CTRL_H
#include "xil_types.h"
#define SWEEP_BASE_ADDRESS 0x40002000U
#define SWEEP_DEVICE_ID 0x53574d31U
void SweepCtrl_Init(void);
int SweepCtrl_Command(const char *line);
void SweepCtrl_Poll(u32 now_ms);
int SweepCtrl_IsScanning(void);
int SweepCtrl_IsStopping(void);
int SweepCtrl_HardwarePiActive(void);
/* E0 is queued first: cancel any automatic owner and keep output closed. */
void SweepCtrl_StopOutput(void);
void SweepCtrl_ManualCommand(char code);
void SweepCtrl_StopForPll(void);
void SweepCtrl_OverrideLoopStatus(u32 words[16]);
#endif
