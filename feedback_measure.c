#if defined(__GNUC__)
#pragma GCC optimize ("O2")
#endif
#include "feedback_measure.h"
#include <string.h>

static int canonical(int code, int twos)
{
    return twos ? ((code ^ 2048) & 4095) : code;
}
static int sample(uint32_t word, const FeedbackCalibration *cal)
{
    return canonical((int)((word >> (cal->channel_b ? 12 : 0)) & 4095U),
                     cal->twos_complement);
}
int FeedbackMeasure_Calibrate(FeedbackCalibration *cal, int c0, int v0,
                             int c1, int v1, int twos)
{
    int a, b;
    if (!cal || c0 < 0 || c0 > 4095 || c1 < 0 || c1 > 4095 ||
        v0 < -20000 || v0 > 20000 || v1 < -20000 || v1 > 20000 ||
        (twos != 0 && twos != 1) || v0 == v1) return 0;
    a = canonical(c0, twos); b = canonical(c1, twos);
    if (a == b || (a-b > -16 && a-b < 16)) return 0;
    cal->code0 = a; cal->code1 = b; cal->mv0 = v0; cal->mv1 = v1;
    cal->twos_complement = twos; cal->valid = 1;
    return 1;
}
int FeedbackMeasure_WindowSamples(uint32_t ftw, uint32_t *count)
{
    uint64_t n;
    /* First version: ordinary sine, 1 kHz..5 MHz at the nominal 65 MHz clock. */
    if (!count || ftw < 66077U || ftw > 330382100U) return 0;
    n = ((3ULL << 32) + ftw - 1U) / ftw + 64U;
    if (n < 2048U) n = 2048U;
    if (n > FEEDBACK_MAX_SAMPLES) return 0;
    *count = (uint32_t)n;
    return 1;
}
int FeedbackMeasure_Calculate(const uint32_t *words, uint32_t count,
                             const FeedbackCalibration *cal, FeedbackMeasurement *out)
{
    uint32_t i, first = 0, last = 0, crossings = 0, selected_count;
    int low = 4095, high = 0, mid, lower, armed = 0, x;
    uint64_t sum = 0;
    float mean, slope, span;
    if (!words || !cal || !out || count < 16U) return 0;
    memset(out, 0, sizeof(*out));
    for (i=0; i<count; ++i) {
        x = sample(words[i], cal);
        if (x < low) low = x;
        if (x > high) high = x;
        sum += (uint32_t)x;
    }
    out->minimum = (uint32_t)low; out->maximum = (uint32_t)high;
    out->clipped = low <= 4 || high >= 4091;
    /* Integrate between rising crossings, avoiding a partial-period DC bias. */
    if (high - low >= 4) {
        mid = (high + low) / 2;
        lower = mid - (high - low) / 6;
        for (i=0; i<count; ++i) {
            x = sample(words[i], cal);
            if (x <= lower) armed = 1;
            if (armed && x >= mid) {
                if (crossings == 0) first = i;
                last = i; ++crossings; armed = 0;
            }
        }
        if (crossings < 2 || last <= first) {
            out->mean_q16=(uint32_t)((sum*65536ULL)/count);
            return 1;
        }
        sum = 0; low = 4095; high = 0;
        for (i=first; i<last; ++i) {
            x = sample(words[i], cal); sum += (uint32_t)x;
            if (x < low) low = x;
            if (x > high) high = x;
        }
        selected_count = last - first;
        out->cycles = crossings - 1U;
    } else selected_count = count; /* DC output; a VPP=0 target is supported. */
    mean = (float)sum / (float)selected_count;
    out->mean_q16 = (uint32_t)(mean * 65536.0f + 0.5f);
    if (!cal->valid) return 1; /* Raw diagnostic values are still useful. */
    slope = (float)(cal->mv1 - cal->mv0) / (float)(cal->code1 - cal->code0);
    span = slope * (float)(high-low);
    out->vpp_mv = span < 0.0f ? -span : span;
    out->offset_mv = (float)cal->mv0 + (mean - (float)cal->code0) * slope;
    out->valid = !out->clipped;
    return 1;
}
