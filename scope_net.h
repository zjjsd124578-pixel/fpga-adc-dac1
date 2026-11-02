#ifndef SCOPE_NET_H
#define SCOPE_NET_H

#include "xil_types.h"

#define SCOPE_NET_PORT          6001U
#define SCOPE_PREVIEW_POINTS    4096U

#define SCOPE_CMD_ARM           (1U << 0)
#define SCOPE_CMD_TRIGGER       (1U << 1)
#define SCOPE_CMD_PAUSE         (1U << 2)
#define SCOPE_CMD_DUMP          (1U << 3)
#define SCOPE_CMD_WINDOW        (1U << 4)

int ScopeNet_Start(void);
void ScopeNet_Poll(void);
u32 ScopeNet_TakeCommands(void);
void ScopeNet_GetWindow(u32 *view_us, u32 *before_us, u32 *after_us);
int ScopeNet_IsConnected(void);
int ScopeNet_IsDumpBusy(void);
int ScopeNet_QueuePreview(u64 first_sample, const u32 *words, u32 count,
                          u32 stride, u32 flags);
void ScopeNet_ReportStatus(u32 flags, u64 total_samples, u32 valid_samples);
void ScopeNet_BeginDump(u64 first_sample, u32 count, u32 flags);

void ScopeNet_ReportLoop(const u32 words[16]);
/* SCP1 type 8, 64-byte frozen DPLL result; header flags carry AXI status. */
void ScopeNet_ReportPll(const u32 words[16], u32 flags);
/* SCP1 types 9..12, bounded queue. Returns 0 for backpressure/no client. */
int ScopeNet_QueueMeasure(u32 type,u32 flags,const u32 *words,u32 count);
#endif
