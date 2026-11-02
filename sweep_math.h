#ifndef SWEEP_MATH_H
#define SWEEP_MATH_H
#include <stdint.h>
#define SWEEP_MAX_POINTS 512U
enum {SWEEP_LP=0,SWEEP_HP=1,SWEEP_BP=2};
enum {SWEEP_FOUND_LOW=1,SWEEP_FOUND_HIGH=2,SWEEP_FOUND_PEAK=4,SWEEP_BOUNDARY=8};
typedef struct { uint32_t hz;float gain,phase_deg;uint32_t flags; } SweepCurvePoint;
typedef struct {
    uint32_t flags;float cutoff_hz,peak_hz,low_hz,high_hz,bandwidth_hz,q;
    uint32_t refine_low,refine_high;
} SweepFeatures;
uint32_t SweepMath_Frequency(uint32_t first,uint32_t last,unsigned index,unsigned count,int logarithmic);
int SweepMath_Baseline(const SweepCurvePoint *base,unsigned count,uint32_t hz,float *gain,float *phase);
int SweepMath_Find(const SweepCurvePoint *points,unsigned count,int mode,SweepFeatures *features);
void SweepMath_Insert(SweepCurvePoint *points,unsigned *count,const SweepCurvePoint *point);
float SweepMath_WrapPhase(float degrees);
#endif
