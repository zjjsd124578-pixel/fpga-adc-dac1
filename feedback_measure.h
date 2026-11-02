#ifndef FEEDBACK_MEASURE_H
#define FEEDBACK_MEASURE_H
#include <stdint.h>
#define FEEDBACK_MAX_SAMPLES 196608U
typedef struct {
    int valid, twos_complement, channel_b, dc_coupled;
    int code0, code1, mv0, mv1;
} FeedbackCalibration;
typedef struct {
    float vpp_mv, offset_mv;
    uint32_t minimum, maximum, mean_q16, cycles;
    int valid, clipped;
} FeedbackMeasurement;
int FeedbackMeasure_Calibrate(FeedbackCalibration *cal, int code0, int mv0,
                             int code1, int mv1, int twos);
int FeedbackMeasure_WindowSamples(uint32_t ftw, uint32_t *count);
int FeedbackMeasure_Calculate(const uint32_t *words, uint32_t count,
                             const FeedbackCalibration *cal, FeedbackMeasurement *out);
#endif
