#if defined(__GNUC__)
#pragma GCC optimize ("O2")
#endif
#include "amplitude_loop.h"
#include "feedback_defaults.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static FeedbackCalibration calibration;
static FeedbackCalibration channel_calibration[2];
static int channel_default[2];
static uint32_t calibration_generation;
static FeedbackMeasurement measured;
static int run, amp_enabled, offset_configured, seeded, state, report;
static int target_vpp, target_offset, dac_span;
static int adc_default, dac_default, offset_pending, offset_limited;
static int manual_bias_pending;
static int32_t manual_bias_q16;
static float gain, bias, previous_amp_error;
static float observed_span;
static int observed_span_valid;
static float absolute(float x) { return x < 0 ? -x : x; }
static float bound(float x, float a, float b) { return x<a ? a : x>b ? b : x; }
static void load_adc_defaults(void)
{
    calibration.valid=FeedbackMeasure_Calibrate(&calibration,
        FEEDBACK_ADC_CODE0,FEEDBACK_ADC_MV0,FEEDBACK_ADC_CODE1,
        FEEDBACK_ADC_MV1,FEEDBACK_ADC_FORMAT);
    adc_default=1;
}
static int integer(const char **p, int *value)
{
    char *end; long n;
    while (**p == ' ' || **p == '\t') ++*p;
    if (!(**p == '+' || **p == '-' || (**p >= '0' && **p <= '9'))) return 0;
    n = strtol(*p, &end, 10);
    if (end == *p || n < -50000L || n > 50000L) return 0;
    *p = end; *value = (int)n; return 1;
}
static int finished(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return *p == 0;
}
void AmplitudeLoop_Init(void)
{
    memset(&calibration,0,sizeof(calibration)); calibration.channel_b = 1;
    load_adc_defaults();
    channel_calibration[0]=channel_calibration[1]=calibration;
    channel_calibration[0].channel_b=0;channel_calibration[1].channel_b=1;
    channel_default[0]=channel_default[1]=1;calibration_generation=0;
    memset(&measured,0,sizeof(measured));
    run=amp_enabled=offset_configured=seeded=report=0;
    state=LOOP_DISABLED; target_vpp=target_offset=0;
    dac_span=FEEDBACK_DAC_SIGNED_SPAN_MV; dac_default=1;
    gain=bias=previous_amp_error=0;
    observed_span=0; observed_span_valid=0;
    manual_bias_pending=offset_pending=offset_limited=0; manual_bias_q16=0;
}
int AmplitudeLoop_Command(const char *line)
{
    char name[20]; unsigned int n=0;
    const char *p=line; int v[5], i;
    if (!line) return -1;
    while (*p == ' ' || *p == '\t') ++p;
    while (isalpha((unsigned char)*p)) {
        if (n+1U >= sizeof(name)) return 0;
        name[n++]=(char)toupper((unsigned char)*p++);
    }
    name[n]=0;
    if (strcmp(name,"VPP") && strcmp(name,"OFFSET") && strcmp(name,"LOOP") &&
        strcmp(name,"FBCH") && strcmp(name,"CALADC") && strcmp(name,"CALDAC") &&
        strcmp(name,"DCCOUPLED") && strcmp(name,"LOOPSTATUS") && strcmp(name,"BIAS") && strcmp(name,"ADCDEFAULT")) return 0;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '=' || *p == ':') ++p;
    if (!strcmp(name,"LOOPSTATUS")) {
        if (!finished(p)) return -1;
        report=1; return 1;
    }
    if (!strcmp(name,"ADCDEFAULT")) {
        if(!finished(p))return -1;
        load_adc_defaults();channel_calibration[calibration.channel_b]=calibration;
        channel_default[calibration.channel_b]=1;++calibration_generation;
        seeded=0;measured.valid=0;observed_span_valid=0;
    } else if (!strcmp(name,"CALADC")) {
        for(i=0;i<5;++i) {
            if (!integer(&p,&v[i]) || (i<4 && *p!=' ' && *p!='\t')) return -1;
        }
        if (!finished(p) || !FeedbackMeasure_Calibrate(&calibration,v[0],v[1],v[2],v[3],v[4])) return -1;
        adc_default=0; seeded=0; measured.valid=0; observed_span_valid=0;
        channel_calibration[calibration.channel_b]=calibration;
        channel_default[calibration.channel_b]=0;++calibration_generation;
    } else {
        if (!integer(&p,&v[0]) || !finished(p)) return -1;
        if (!strcmp(name,"BIAS")) {
            if(v[0]<-127 || v[0]>126) return -1;
            manual_bias_q16=(int32_t)v[0]*65536; manual_bias_pending=1;
            offset_configured=offset_pending=offset_limited=0;
            run=0; seeded=0; state=LOOP_DISABLED;
        } else if (!strcmp(name,"VPP")) {
            if(v[0]<0 || v[0]>20000) return -1;
            target_vpp=v[0]; amp_enabled=1; run=1;
        } else if (!strcmp(name,"OFFSET")) {
            if(v[0]<-20000 || v[0]>20000) return -1;
            target_offset=v[0]; offset_configured=offset_pending=1;
            manual_bias_pending=0;
            /* A one-shot DAC setting: never starts a DC PI or requires ADC. */
        } else if (!strcmp(name,"LOOP")) {
            if(v[0]!=0 && v[0]!=1) return -1;
            run=v[0]; if(!run) state=LOOP_DISABLED;
        } else if (!strcmp(name,"FBCH")) {
            if(v[0]!=0 && v[0]!=1) return -1;
            if(calibration.channel_b != v[0]) {
                channel_calibration[calibration.channel_b]=calibration;
                channel_default[calibration.channel_b]=adc_default;
                calibration=channel_calibration[v[0]];adc_default=channel_default[v[0]];
                seeded=0; measured.valid=0; observed_span_valid=0;
            }
        } else if (!strcmp(name,"CALDAC")) {
            if(v[0]>-100 && v[0]<100) return -1;
            dac_span=v[0]; dac_default=0; seeded=0; observed_span_valid=0;
            if(offset_configured) offset_pending=1;
        } else {
            if(v[0]!=0 && v[0]!=1) return -1;
            calibration.dc_coupled=v[0]; /* Retained diagnostic flag only. */
        }
    }
    previous_amp_error=0;
    return 1;
}
void AmplitudeLoop_ManualCommand(char code)
{
    if (code != 'F') {
        run=0; seeded=0; state=LOOP_DISABLED;
        /* Legacy settings do not overwrite DAC bias. Retain OFFSET until the
         * manual queue drains, including OFFSET0 followed by E1 in one packet. */
        manual_bias_pending=0;
    }
    observed_span_valid=0;
    previous_amp_error=0;
}
const FeedbackCalibration *AmplitudeLoop_Calibration(void) { return &calibration; }
void AmplitudeLoop_GetChannelCalibration(unsigned channel,FeedbackCalibration *out,int *uses_default)
{
    if(channel>1 || !out || !uses_default)return;
    if((int)channel==calibration.channel_b){*out=calibration;*uses_default=adc_default;}
    else {*out=channel_calibration[channel];*uses_default=channel_default[channel];}
}
uint32_t AmplitudeLoop_CalibrationGeneration(void){return calibration_generation;}
int AmplitudeLoop_Ready(int capture_running)
{
    if (!run || !amp_enabled) {state=LOOP_DISABLED; return 0;}
    if(!calibration.valid) {state=LOOP_WAIT_ADC_CAL; return 0;}
    if(!dac_span) {state=LOOP_WAIT_DAC_CAL; return 0;}
    if(!capture_running) {state=LOOP_WAIT_CAPTURE; return 0;}
    return 1;
}
int AmplitudeLoop_NeedsSeed(void) { return !seeded; }
static void project(uint32_t *g, int32_t *b)
{
    float center, ceiling;
    bias=bound(bias,-127.0f,126.0f);
    center=128.0f+bias;
    ceiling=center/128.0f;
    if((255.0f-center)/127.0f < ceiling) ceiling=(255.0f-center)/127.0f;
    gain=bound(gain,0.0f,bound(ceiling,0.0f,1.0f));
    *b=(int32_t)(bias*65536.0f);
    *g=(uint32_t)(gain*65536.0f);
    {
        int32_t c=8388608+*b;
        uint32_t hi=(uint32_t)c/128U;
        uint32_t other=(uint32_t)(16711680-c)/127U;
        if(other<hi) hi=other;
        if(*g>hi) *g=hi;
    }
}
void AmplitudeLoop_Seed(uint32_t *g,int32_t *b)
{
    gain=bound((float)target_vpp/absolute((float)dac_span),0,1);
    bias=(float)*b/65536.0f; project(g,b); seeded=1;
    previous_amp_error=0; state=LOOP_WAIT_DATA;
    observed_span_valid=0;
}
void AmplitudeLoop_SyncOutput(uint32_t g,int32_t b)
{
    gain=(float)g/65536.0f; bias=(float)b/65536.0f;
    previous_amp_error=0; measured.valid=0;
    observed_span_valid=0;
    if(run && amp_enabled) state=LOOP_WAIT_DATA;
}
void AmplitudeLoop_ResetMeasurement(void)
{
    measured.valid=0; previous_amp_error=0;
    observed_span_valid=0;
    if(run) state=LOOP_WAIT_DATA;
}
void AmplitudeLoop_InvalidateMeasurement(void)
{
    measured.valid=0;
    if(run && amp_enabled) state=LOOP_WAIT_DATA;
}
void AmplitudeLoop_SetState(int s)
{
    state=s;
    if(s==LOOP_WAIT_DATA || s==LOOP_WAIT_CAPTURE || s==LOOP_BAD_SIGNAL || s==LOOP_BAD_FREQUENCY)
        measured.valid=0;
}
int AmplitudeLoop_Update(const FeedbackMeasurement *m,uint32_t *g,int32_t *b)
{
    float ea,span,da=0,tolerance,old_gain,adc_step;
    uint32_t old_g; int32_t old_b;
    measured=*m;
    if(m->clipped) {state=LOOP_ADC_CLIPPED; return 0;}
    if(!m->valid || (target_vpp>0 && !m->cycles)) {
        state=LOOP_BAD_SIGNAL; return 0;
    }
    span=absolute((float)dac_span);
    adc_step=absolute((float)(calibration.mv1-calibration.mv0)/(float)(calibration.code1-calibration.code0));
    /* Normalize the PI against the actual feedback plant, not an assumed DAC
     * voltage. This changes control speed only, never the ADC voltage scale or
     * the meaning of OFFSET/CALDAC. Ignore near-zero/noisy probes. */
    if(m->cycles>=2 && gain>=0.04f && m->vpp_mv>=adc_step*8.0f) {
        float observed=m->vpp_mv/gain;
        if(!observed_span_valid) { observed_span=observed; observed_span_valid=1; }
        else observed_span+=0.25f*(observed-observed_span);
    }
    if(observed_span_valid) span=observed_span;
    tolerance=span/255.0f*0.6f;
    if(tolerance<3.0f) tolerance=3.0f;
    if(tolerance<adc_step) tolerance=adc_step;
    ea=(float)target_vpp-m->vpp_mv;
    if(absolute(ea)>tolerance) {
        da=(0.12f*(ea-previous_amp_error)+0.25f*ea)/span;
        previous_amp_error=ea;
    } else previous_amp_error=0;
    project(&old_g,&old_b); old_gain=gain;
    gain+=bound(da,-0.04f,0.04f);
    if(target_vpp==0) gain=0;
    /* Bias stays fixed: measured DC is reported, never used as a control error. */
    project(g,b);
    state=(absolute(ea)>tolerance && (gain==0 || gain<old_gain+bound(da,-0.04f,0.04f)-0.00001f)) ? LOOP_LIMIT : LOOP_RUNNING;
    return *g!=old_g || *b!=old_b;
}
int AmplitudeLoop_TakeReport(void) { int r=report; report=0; return r; }
void AmplitudeLoop_GetStatus(uint32_t w[16])
{
    w[0]=1; w[1]=(uint32_t)state;
    /* Keep legacy DC-loop bit 2 clear. New bits 8/9 identify nominal defaults,
     * bit 10 offset clipping and bit 11 open-loop offset configuration. */
    w[2]=(uint32_t)(run | amp_enabled<<1 | calibration.valid<<3 |
                  calibration.dc_coupled<<4 | (dac_span!=0)<<5 | measured.valid<<6 | measured.clipped<<7 |
                  adc_default<<8 | dac_default<<9 | offset_limited<<10 | offset_configured<<11);
    w[3]=(uint32_t)calibration.channel_b; w[4]=(uint32_t)target_vpp;
    w[5]=(uint32_t)target_offset;
    w[6]=(uint32_t)(int32_t)measured.vpp_mv; w[7]=(uint32_t)(int32_t)measured.offset_mv;
    w[8]=measured.minimum; w[9]=measured.maximum; w[10]=measured.mean_q16;
    w[11]=measured.cycles; w[12]=(uint32_t)(gain*65536.0f);
    w[13]=(uint32_t)(int32_t)(bias*65536.0f); w[14]=(uint32_t)dac_span;
    w[15]=(uint32_t)calibration.twos_complement;
}
int AmplitudeLoop_State(void) { return state; }
void AmplitudeLoop_Diagnostic(const FeedbackMeasurement *m) { measured=*m; }
int AmplitudeLoop_TakeManualBias(int32_t *b)
{
    if(!manual_bias_pending) return 0;
    *b=manual_bias_q16; manual_bias_pending=0; return 1;
}
int AmplitudeLoop_TakeOpenLoopOffset(int32_t *b)
{
    float desired, applied;
    if(!offset_pending || !dac_span) return 0;
    desired=(float)target_offset*255.0f/(float)dac_span;
    applied=bound(desired,-127.0f,126.0f);
    offset_limited=desired!=applied;
    *b=(int32_t)(applied*65536.0f); offset_pending=0;
    return 1;
}
