#include "sweep_math.h"
#include <math.h>
#include <string.h>
float SweepMath_WrapPhase(float x) {while(x>180.0f)x-=360.0f;while(x<=-180.0f)x+=360.0f;return x;}
uint32_t SweepMath_Frequency(uint32_t first,uint32_t last,unsigned i,unsigned n,int logarithmic)
{
    double t,value;
    if(n<2 || first>last)return first;
    if(i==0)return first;
    if(i>=n-1)return last;
    t=(double)i/(double)(n-1);
    value=logarithmic ? (double)first*exp(log((double)last/first)*t) : first+((double)last-first)*t;
    return (uint32_t)(value+0.5);
}
int SweepMath_Baseline(const SweepCurvePoint *b,unsigned n,uint32_t hz,float *gain,float *phase)
{
    unsigned i;float t;
    if(!b || n<2 || !gain || !phase || hz<b[0].hz || hz>b[n-1].hz)return 0;
    for(i=1;i<n;++i)if(hz<=b[i].hz) {
        if(b[i-1].gain<=0 || b[i].gain<=0 || b[i].hz<=b[i-1].hz)return 0;
        t=logf((float)hz/b[i-1].hz)/logf((float)b[i].hz/b[i-1].hz);
        *gain=expf(logf(b[i-1].gain)+t*logf(b[i].gain/b[i-1].gain));
        *phase=b[i-1].phase_deg+t*(b[i].phase_deg-b[i-1].phase_deg);return 1;
    }
    return 0;
}
void SweepMath_Insert(SweepCurvePoint *p,unsigned *n,const SweepCurvePoint *x)
{
    unsigned i;
    for(i=0;i<*n && p[i].hz<x->hz;++i){}
    if(i<*n && p[i].hz==x->hz){p[i]=*x;return;}
    if(*n>=SWEEP_MAX_POINTS)return;
    memmove(p+i+1,p+i,(*n-i)*sizeof(*p));p[i]=*x;++*n;
}
static int cross(const SweepCurvePoint *a,const SweepCurvePoint *b,float threshold,int rising,float *hz)
{
    float ga,gb,t;
    if(!(a->flags&1) || !(b->flags&1) || a->gain<=0 || b->gain<=0 || a->hz>=b->hz)return 0;
    ga=20.0f*log10f(a->gain);gb=20.0f*log10f(b->gain);
    if(rising ? !(ga<=threshold && gb>=threshold) : !(ga>=threshold && gb<=threshold))return 0;
    if(fabsf(gb-ga)<1e-8f)return 0;
    t=(threshold-ga)/(gb-ga);*hz=expf(logf((float)a->hz)+t*logf((float)b->hz/a->hz));return 1;
}
int SweepMath_Find(const SweepCurvePoint *p,unsigned n,int mode,SweepFeatures *f)
{
    unsigned i,j=0,left=0,right=n?n-1:0;float threshold=-3.01029995664f;
    memset(f,0,sizeof(*f));
    if(n<3 || mode<0 || mode>2)return 0;
    if(mode!=SWEEP_BP) {
        /* This buffered passive RC board has unity passband after through
         * correction. Using the first point as unity biases low-edge cutoffs. */
        for(i=1;i<n;++i)if(cross(p+i-1,p+i,threshold,mode==SWEEP_HP,&f->cutoff_hz)) {
            f->flags=mode==SWEEP_LP ? SWEEP_FOUND_HIGH : SWEEP_FOUND_LOW;
            left=i>1?i-2:0;right=i+1<n?i+1:n-1;
            f->refine_low=p[left].hz;f->refine_high=p[right].hz;return 1;
        }
        f->flags=SWEEP_BOUNDARY;return 0;
    }
    for(i=0;i<n;++i)if((p[i].flags&1) && (!(p[j].flags&1) || p[i].gain>p[j].gain))j=i;
    if(j==0 || j==n-1 || !(p[j].flags&1) || p[j].gain<=0){f->flags=SWEEP_BOUNDARY;return 0;}
    f->flags=SWEEP_FOUND_PEAK;f->peak_hz=(float)p[j].hz;
    // Phase-zero crossing supplies an independent resonance location.
    for(i=j>1?j-1:1;i<n && i<=j+2;++i) {
        float pa=p[i-1].phase_deg,pb=p[i].phase_deg;
        if((p[i-1].flags&1) && (p[i].flags&1) && pa>=0 && pb<=0 && pa-pb>0.001f && pa-pb<180) {
            f->peak_hz=expf(logf((float)p[i-1].hz)+(pa/(pa-pb))*logf((float)p[i].hz/p[i-1].hz));break;
        }
    }
    threshold=20.0f*log10f(p[j].gain)-3.01029995664f;
    left=j>0?j-1:j;right=j+1<n?j+1:j;
    for(i=j;i>0;--i)if(cross(p+i-1,p+i,threshold,1,&f->low_hz)) {
        f->flags|=SWEEP_FOUND_LOW;left=i>1?i-2:0;break;
    }
    for(i=j+1;i<n;++i)if(cross(p+i-1,p+i,threshold,0,&f->high_hz)) {
        f->flags|=SWEEP_FOUND_HIGH;right=i+1<n?i+1:n-1;break;
    }
    if((f->flags&3)==3) {
        f->bandwidth_hz=f->high_hz-f->low_hz;
        if(f->bandwidth_hz>0)f->q=f->peak_hz/f->bandwidth_hz;
    } else f->flags|=SWEEP_BOUNDARY;
    f->refine_low=p[left].hz;f->refine_high=p[right].hz;
    return 1;
}
