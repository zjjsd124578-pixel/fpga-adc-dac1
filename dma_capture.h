#ifndef DMA_CAPTURE_H
#define DMA_CAPTURE_H

#include "xil_types.h"

/* ADC 双通道样本经 adc_axis_packer 补齐为 32 位，即每点 4 字节。 */
#define DMA_BYTES_PER_SAMPLE       4U

/* 必须与 adc_axis_packer.v 中的 FRAME_SAMPLES 保持一致。 */
#define DMA_FRAME_SAMPLES          4096U
#define DMA_BYTES_PER_FRAME        (DMA_FRAME_SAMPLES * DMA_BYTES_PER_SAMPLE)

/* DDR 最大保留区仍受 1 GiB 地址映射限制，实际环长度由时间窗决定。 */
#define DMA_BD_COUNT               128U
#define DMA_MIN_RECORD_BLOCK_COUNT 128U
#define DMA_MAX_RECORD_BLOCK_COUNT 55600U
#define DMA_RECORD_BASE_ADDR       0x04000000U
#define DMA_SAMPLE_RATE_HZ         65000000U
#define DMA_MAX_CAPTURE_SAMPLES    227500000U

/* 初始化 AXI DMA、S2MM SG 描述符环和 PS 中断控制器。 */
int DmaCapture_Init(void);

/* 设置触发点前后要保留的原始点数；仅可在停止采集后调用。 */
int DmaCapture_Configure(u32 pre_samples, u32 post_samples);

/* 重建描述符环并开始一次新的 DDR 环形采集；调用后才能置 arm=1。 */
int DmaCapture_Start(void);

/*
 * PL 的 capture_done 出现后，等待 FIFO/DMA 排空并冻结 DDR。
 * 返回 1 表示本次采集已结束，返回 0 表示仍在采集。
 */
int DmaCapture_IsDone(void);

/* 返回 1 表示 DMA 出错。 */
int DmaCapture_HasError(void);

/* 记录区在链接器保留范围之外，不参与 .bss 清零。 */
u32 DmaCapture_GetBufferAddr(void);
u32 DmaCapture_GetBufferSize(void);
u32 DmaCapture_GetCompletedFrames(void);
u64 DmaCapture_GetTotalSamples(void);
u32 DmaCapture_GetPretriggerSamples(void);
u32 DmaCapture_GetPosttriggerSamples(void);
u32 DmaCapture_GetSampleAddr(u64 absolute_sample);
void DmaCapture_Invalidate(u32 addr, u32 bytes);

/* 从最新完成的 2 ms 记录中抽取一帧；采集不足时返回 0。 */
int DmaCapture_CopyPreview(u32 *destination, u32 point_count,
                           u32 stride, u64 *first_sample);

/* 最近一个已经由 DMA 完成的缓冲块首地址，便于后续整理环形数据顺序。 */
u32 DmaCapture_GetLastCompletedAddr(void);

/* 最近完成块的实际有效字节数。最后一帧可能小于 16 KB。 */
u32 DmaCapture_GetLastCompletedLength(void);

/* 当前环形缓冲中包含有效数据的块数，以及按时间排序的最旧块地址。 */
u32 DmaCapture_GetValidBlockCount(void);
u32 DmaCapture_GetOldestBufferAddr(void);

/* Continuous completed samples, never preview decimation. Returns 0 when
 * data is insufficient, capture is faulty, or the DMA could overwrite it. */
int DmaCapture_CopyRaw(u32 *destination, u32 count, u64 *first_sample);
#endif
