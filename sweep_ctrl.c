#include "sweep_ctrl.h"
#include "sweep_math.h"
#include "dds_ctrl.h"
#include "dpll_ctrl.h"
#include "amplitude_loop.h"
#include "scope_net.h"
#include "xil_io.h"
#include "xil_printf.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define FS_HZ 65000000U
#define MAX_HZ 1000000U
#define REG_CTRL 4U
#define REG_COMMIT 0x20U
#define REG_STATUS 0x24U
#define REG_SNAP 0x28U
#define REG_GEN 0x2cU
#define POINT_VALID 1U
#define POINT_BASELINE 2U
#define POINT_WEAK 4U
#define POINT_CLIP 8U
enum {ROLE_NONE,ROLE_BASE,ROLE_SCAN,ROLE_MANUAL,ROLE_EXPORT};
enum {IDLE,PROBE,PREPARE,CONFIGURE,WAIT_CONFIG,SET_FREQUENCY,WAIT_FREQUENCY,
      MEASURE,EMIT,ANALYZE,STOP_DDS,STOP_PI,EXPORT_META,EXPORT_POINTS,EXPORT_END};
typedef struct {u32 first,last,coarse,fine,target_mv,mode;} ScanSettings;
static ScanSettings settings;
static SweepCurvePoint baseline[SWEEP_MAX_POINTS],curve[SWEEP_MAX_POINTS];
static unsigned baseline_count,curve_count,point_index,point_total;
static u32 baseline_signature,baseline_target,import_expected,import_signature;
static int baseline_valid,importing,initialized,device_ready,role,state,prepared;
static int bias_cleaned,stop_preserve,zero_output,fine_pass,phase_stage,pending_report;
static u32 stage_first,stage_last,expected_ftw,expected_gen,last_seq,point_started,start_ms;
static u32 now_r,fault,manual_target,manual_signature,last_manual_dds_gen,read_after_ms;
static unsigned manual_channel,export_index;
static int restore_channel=-1;
static u32 last_words[24],emit_words[24],summary_words[16],meta_words[16];
static SweepFeatures features;

static u32 read_reg(u32 offset){return Xil_In32((UINTPTR)SWEEP_BASE_ADDRESS+offset);}
static void write_reg(u32 offset,u32 value){Xil_Out32((UINTPTR)SWEEP_BASE_ADDRESS+offset,value);}
static u32 ftw_from_hz(u32 hz){return (u32)((((u64)hz<<32)+FS_HZ/2U)/FS_HZ);}
static u32 hz_from_ftw(u32 ftw){return (u32)(((u64)ftw*FS_HZ+(1ULL<<31))>>32);}
static u32 rounded(float x){return x<=0 ? 0 : (x>=4294967040.0f ? 0xffffffffU : (u32)(x+0.5f));}
static s32 signed_round(float x){return (s32)(x+(x>=0?0.5f:-0.5f));}
static int integer(const char **p,s32 *value)
{
    char *end;long n;while(**p==' '||**p=='\t')++*p;
    if(!(**p=='+'||**p=='-'||isdigit((unsigned char)**p)))return 0;
    n=strtol(*p,&end,10);if(end==*p || n<-2000000000L || n>2000000000L)return 0;
    *p=end;*value=(s32)n;return 1;
}
static int end(const char *p){while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')++p;return !*p;}
static u32 signature(void)
{
    FeedbackCalibration c;int defaults;unsigned ch,j;u32 hash=2166136261U,words[6];
    for(ch=0;ch<2;++ch){
        AmplitudeLoop_GetChannelCalibration(ch,&c,&defaults);
        words[0]=(u32)c.code0;words[1]=(u32)c.mv0;words[2]=(u32)c.code1;
        words[3]=(u32)c.mv1;words[4]=(u32)c.twos_complement;words[5]=(u32)defaults;
        for(j=0;j<6;++j){hash^=words[j];hash*=16777619U;}
    }
    return hash;
}
static float slope(unsigned channel,int whole_board)
{
    FeedbackCalibration c;int defaults;float result;
    AmplitudeLoop_GetChannelCalibration(channel,&c,&defaults);
    if(!c.valid || c.code1==c.code0)return 0;
    result=fabsf((float)(c.mv1-c.mv0)/(c.code1-c.code0));
    /* Existing defaults describe the ADC SMA input. Explicit CALADC points
     * for this application are measured at VIN and include the whole path. */
    if(whole_board && defaults)result*=5067.0f/5020.0f;
    return result;
}
static int queue_dds(char code,u32 value){DdsCommand c;c.code=code;c.value=value;return DdsCtrl_QueueCommand(&c)==DDS_CTRL_OK;}
static void request_stop(int preserve)
{
    if(state!=STOP_DDS && state!=STOP_PI) {
        stop_preserve=preserve;state=STOP_DDS;prepared=0;
    } else if(!preserve) {
        /* Closing/cancelling wins over a later LOOP0 or repeated stop.
         * Do not restart an already committed asynchronous exit. */
        stop_preserve=0;
    }
    AmplitudeLoop_Command("LOOP0");
}
static int report(u32 type,const u32 *words,unsigned n)
{
    return !ScopeNet_IsConnected() || ScopeNet_QueueMeasure(type,0,words,n);
}
static void fill_meta(void)
{
    FeedbackCalibration c;int defaults;unsigned ch;memset(meta_words,0,sizeof(meta_words));
    meta_words[0]=1;meta_words[1]=signature();meta_words[2]=baseline_target;meta_words[3]=baseline_count;
    for(ch=0;ch<2;++ch){
        unsigned j=4+ch*5;AmplitudeLoop_GetChannelCalibration(ch,&c,&defaults);
        /* CALADC consumes raw codes; internal calibration stores canonical
         * offset-binary codes. Convert back for a lossless file restore. */
        meta_words[j]=(u32)(c.twos_complement?c.code0^2048:c.code0);meta_words[j+1]=(u32)c.mv0;
        meta_words[j+2]=(u32)(c.twos_complement?c.code1^2048:c.code1);meta_words[j+3]=(u32)c.mv1;meta_words[j+4]=(u32)c.twos_complement;
        if(defaults)meta_words[14]|=1U<<ch;
    }
    meta_words[15]=(u32)baseline_valid;
}
static void fill_summary(void)
{
    float frequency=role==ROLE_SCAN ? (settings.mode==SWEEP_BP?features.peak_hz:features.cutoff_hz) : 0;
    float cap_pf=0;
    if(frequency>0)cap_pf=settings.mode==SWEEP_BP ?
        1.0e12f/(39.4784176044f*0.0033f*frequency*frequency) : 1.0e12f/(6.28318530718f*1600.0f*frequency);
    memset(summary_words,0,sizeof(summary_words));
    summary_words[0]=1;summary_words[1]=role==ROLE_BASE?0:(role==ROLE_EXPORT?4:role==ROLE_MANUAL?3:2);
    summary_words[2]=settings.mode;summary_words[3]=features.flags|(fault?0x80000000U:0);
    summary_words[4]=curve_count;summary_words[5]=now_r-start_ms;
    summary_words[6]=rounded(features.cutoff_hz);summary_words[7]=rounded(features.peak_hz);
    summary_words[8]=rounded(features.low_hz);summary_words[9]=rounded(features.high_hz);
    summary_words[10]=rounded(features.bandwidth_hz);summary_words[11]=rounded(features.q*65536);
    summary_words[12]=rounded(cap_pf);summary_words[13]=fault;
    summary_words[14]=settings.first;summary_words[15]=settings.last;
}
static int configure_measurement(void)
{
    FeedbackCalibration c;int defaults;u32 gain,min_gain=1024,max_gain=65536,target_codes,control;
    s32 bias;float mv_per_code;unsigned channel=role==ROLE_MANUAL?manual_channel:0;
    if(read_reg(REG_STATUS)&1)return 0;
    AmplitudeLoop_GetChannelCalibration(channel,&c,&defaults);
    (void)defaults;mv_per_code=slope(channel,role!=ROLE_MANUAL);
    if(mv_per_code<=0)return -1;
    target_codes=rounded((role==ROLE_MANUAL?manual_target:settings.target_mv)*65536.0f/mv_per_code);
    if(target_codes<65536 || target_codes>0x0fff0000U)return -1;
    DdsCtrl_GetFeedback(&gain,&bias);
    if(last_words[16]&1)gain=last_words[17];
    if(role==ROLE_MANUAL) {
        s32 center=8388608+bias;
        if(center<0 || center>16711680)return -1;
        max_gain=(u32)center/128U;
        if((u32)(16711680-center)/127U<max_gain)max_gain=(u32)(16711680-center)/127U;
        if(max_gain>65536)max_gain=65536;
        if(max_gain<min_gain)return -1;
    }
    if(gain<min_gain)gain=min_gain;
    if(gain>max_gain)gain=max_gain;
    control=11U|(c.twos_complement?4U:0U)|(channel?16U:0U);
    write_reg(4,control);write_reg(8,0);write_reg(0x0c,phase_stage==2?16:8);
    write_reg(0x10,65000);write_reg(0x14,65000);write_reg(0x18,16777216);
    write_reg(0x1c,8U<<16);
    write_reg(0x80,target_codes);write_reg(0x84,gain);write_reg(0x88,min_gain);write_reg(0x8c,max_gain);
    write_reg(0x90,target_codes/50U);write_reg(0x94,1);write_reg(0x98,1);write_reg(0x9c,2);
    write_reg(REG_COMMIT,1);expected_gen=read_reg(REG_GEN);last_seq=0;
    manual_signature=signature();last_manual_dds_gen=DdsCtrl_GetGeneration();return 1;
}
static int snapshot(u32 words[24])
{
    unsigned i;u32 status=read_reg(REG_STATUS);
    if((status&3)!=2)return 0; // no SNAP write while busy or without a live result
    write_reg(REG_SNAP,1);
    if(!(read_reg(REG_STATUS)&4))return 0;
    for(i=0;i<16;++i)words[i]=read_reg(0x40+4*i);
    for(i=0;i<8;++i)words[16+i]=read_reg(0xc0+4*i);
    return words[1]==expected_gen;
}
static int make_point(const u32 w[24])
{
    SweepCurvePoint point;float a,b,base_gain=1,base_phase=0,gain,phase;
    int valid=(w[3]&0x87U)==0x87U && !(w[3]&0x178U);
    float target=role==ROLE_MANUAL?manual_target:settings.target_mv;
    unsigned channel=role==ROLE_MANUAL?manual_channel:0;
    float feedback=(channel?w[6]:w[5])/65536.0f*slope(channel,role!=ROLE_MANUAL);
    if(!(w[16]&2) || fabsf(feedback-target)>target*0.0205f)return 0;
    a=w[5]/65536.0f*slope(0,role!=ROLE_MANUAL);b=w[6]/65536.0f*slope(1,role!=ROLE_MANUAL);
    if(a<=0)return 0;
    gain=b/a;phase=(s32)w[10]/1000.0f;
    point.hz=hz_from_ftw(w[2]);point.flags=valid?POINT_VALID:0;
    if(role==ROLE_BASE) {
        if(!valid || gain<0.5f || gain>2.0f)return -1;
        point.gain=gain;
        if(baseline_count)phase=baseline[baseline_count-1].phase_deg+SweepMath_WrapPhase(phase-baseline[baseline_count-1].phase_deg);
        point.phase_deg=phase;
        if(baseline_count>=SWEEP_MAX_POINTS)return -1;
        baseline[baseline_count++]=point;
    } else if(role==ROLE_SCAN) {
        if(!SweepMath_Baseline(baseline,baseline_count,point.hz,&base_gain,&base_phase))return -1;
        gain/=base_gain;phase=SweepMath_WrapPhase(phase-base_phase);point.flags|=POINT_BASELINE;
    }
    point.gain=gain;point.phase_deg=phase;
    if(w[3]&0x18)point.flags|=POINT_CLIP;
    if((w[3]&6)!=6)point.flags|=POINT_WEAK;
    if(role!=ROLE_MANUAL)SweepMath_Insert(curve,&curve_count,&point);
    memset(emit_words,0,sizeof(emit_words));
    emit_words[0]=1;emit_words[1]=role==ROLE_BASE?0:role==ROLE_MANUAL?3:(u32)phase_stage;
    emit_words[2]=point_index;emit_words[3]=point_total;emit_words[4]=point.hz;emit_words[5]=point.flags;
    emit_words[6]=rounded(gain*65536);emit_words[7]=(u32)signed_round(gain>1e-6f?20000*log10f(gain):-120000);
    emit_words[8]=(u32)signed_round(phase*1000);emit_words[9]=rounded(a);emit_words[10]=rounded(b);
    emit_words[11]=w[17];emit_words[12]=w[3];emit_words[13]=w[16];emit_words[14]=w[1];emit_words[15]=w[0];
    emit_words[16]=settings.mode;emit_words[17]=signature();emit_words[18]=w[5];emit_words[19]=w[6];
    emit_words[20]=rounded(base_gain*65536);emit_words[21]=(u32)signed_round(base_phase*1000);
    emit_words[22]=w[4];emit_words[23]=w[21];return 1;
}
void SweepCtrl_Init(void)
{
    settings.first=1000;settings.last=1000000;settings.coarse=200;settings.fine=101;settings.target_mv=1000;settings.mode=SWEEP_LP;
    baseline_count=curve_count=0;baseline_valid=importing=device_ready=0;
    role=ROLE_NONE;state=IDLE;initialized=1;manual_target=0;pending_report=0;
    memset(last_words,0,sizeof(last_words));memset(&features,0,sizeof(features));
}
int SweepCtrl_IsScanning(void){return initialized && role!=ROLE_NONE && role!=ROLE_MANUAL && role!=ROLE_EXPORT;}
int SweepCtrl_IsStopping(void){return initialized && role!=ROLE_NONE && (state==STOP_DDS || state==STOP_PI);}
int SweepCtrl_HardwarePiActive(void){return initialized && device_ready && role!=ROLE_NONE && role!=ROLE_EXPORT && state>=WAIT_CONFIG;}
void SweepCtrl_StopOutput(void)
{
    AmplitudeLoop_Command("LOOP0");
    if(role!=ROLE_NONE)request_stop(0);
}
void SweepCtrl_StopForPll(void){if(role==ROLE_MANUAL)request_stop(1);}
void SweepCtrl_ManualCommand(char code)
{
    if(role==ROLE_MANUAL && code!='F')request_stop(0);
}
int SweepCtrl_Command(const char *line)
{
    char name[24];unsigned n=0,i;const char *p=line;s32 v[6];
    if(!initialized)return 0;
    while(*p==' '||*p=='\t')++p;
    while(isalpha((unsigned char)*p)){if(n+1>=sizeof(name))return 0;name[n++]=(char)toupper((unsigned char)*p++);}
    name[n]=0;while(*p==' '||*p=='\t')++p;if(*p=='='||*p==':')++p;
    if(!strcmp(name,"VPP")) {
        if(SweepCtrl_IsScanning() || DpllCtrl_IsRequestedOrActive() || !integer(&p,&v[0]) || !end(p) || v[0]<0 || v[0]>20000)return -1;
        if(v[0]>0 && SweepCtrl_IsStopping())return -1;
        if(AmplitudeLoop_Command(line)<0)return -1;
        AmplitudeLoop_Command("LOOP0");
        manual_target=(u32)v[0];zero_output=manual_target==0;
        if(zero_output){if(role==ROLE_NONE)role=ROLE_MANUAL;request_stop(1);return 1;}
        manual_channel=(unsigned)AmplitudeLoop_Calibration()->channel_b;role=ROLE_MANUAL;state=PROBE;fault=0;
        phase_stage=1;prepared=0;return 1;
    }
    if(!strcmp(name,"LOOP") && (role==ROLE_MANUAL || (role==ROLE_NONE && manual_target))) {
        if(!integer(&p,&v[0])||!end(p)||(v[0]!=0&&v[0]!=1))return -1;
        if(DpllCtrl_IsRequestedOrActive())return -1;
        if(v[0] && SweepCtrl_IsStopping())return -1;
        if(!v[0]) {if(role==ROLE_MANUAL)request_stop(1);}
        else {role=ROLE_MANUAL;manual_channel=(unsigned)AmplitudeLoop_Calibration()->channel_b;state=PROBE;prepared=0;zero_output=0;}
        return 1;
    }
    if(!strcmp(name,"SWCONFIG")) {
        if(role!=ROLE_NONE)return -1;
        for(i=0;i<6;++i)if(!integer(&p,&v[i]))return -1;
        if(!end(p)||v[0]<1000||v[1]>1000000||v[0]>=v[1]||v[2]<16||v[2]>256||v[3]<16||v[3]>256||
           v[2]+v[3]>512||v[4]<100||v[4]>5000||v[5]<0||v[5]>2)return -1;
        settings.first=(u32)v[0];settings.last=(u32)v[1];settings.coarse=(u32)v[2];settings.fine=(u32)v[3];settings.target_mv=(u32)v[4];settings.mode=(u32)v[5];return 1;
    }
    if(!strcmp(name,"SWSTATUS")){if(!end(p))return -1;pending_report=1;return 1;}
    if(!strcmp(name,"LOOP") && SweepCtrl_IsScanning())return -1;
    if(!strcmp(name,"SWSTOP")){if(!end(p))return -1;if(role!=ROLE_NONE)request_stop(0);return 1;}
    if(!strcmp(name,"SWCAL")||!strcmp(name,"SWSTART")) {
        if(!end(p)||role!=ROLE_NONE||DpllCtrl_IsRequestedOrActive())return -1;
        if(!strcmp(name,"SWSTART") && (!baseline_valid || baseline_signature!=signature() || baseline_target!=settings.target_mv ||
            settings.first<baseline[0].hz || settings.last>baseline[baseline_count-1].hz))return -1;
        {
            FeedbackCalibration a,b;int d;
            AmplitudeLoop_GetChannelCalibration(0,&a,&d);AmplitudeLoop_GetChannelCalibration(1,&b,&d);
            if(!a.valid||!b.valid||a.twos_complement!=b.twos_complement)return -1;
        }
        restore_channel=AmplitudeLoop_Calibration()->channel_b;AmplitudeLoop_Command("FBCH0");
        role=!strcmp(name,"SWCAL")?ROLE_BASE:ROLE_SCAN;
        if(role==ROLE_BASE){baseline_valid=0;baseline_count=0;}
        curve_count=0;point_index=0;point_total=settings.coarse;fine_pass=0;phase_stage=1;
        stage_first=settings.first;stage_last=settings.last;state=PROBE;prepared=bias_cleaned=0;
        fault=0;memset(&features,0,sizeof(features));AmplitudeLoop_Command("LOOP0");return 1;
    }
    if(!strcmp(name,"SWEXPORT")) {
        if(!end(p)||role!=ROLE_NONE||!baseline_valid||baseline_signature!=signature())return -1;
        role=ROLE_EXPORT;state=EXPORT_META;export_index=0;fault=0;return 1;
    }
    if(!strcmp(name,"SWIMPORTBEGIN")) {
        if(role!=ROLE_NONE||!integer(&p,&v[0])||!integer(&p,&v[1])||!end(p)||v[0]<2||v[0]>512||v[1]<100||v[1]>5000)return -1;
        baseline_valid=0;baseline_count=0;import_expected=(u32)v[0];baseline_target=(u32)v[1];import_signature=signature();importing=1;return 1;
    }
    if(!strcmp(name,"SWBASE")) {
        SweepCurvePoint x;
        for(i=0;i<3;++i)if(!integer(&p,&v[i]))return -1;
        if(!importing||!end(p)||baseline_count>=import_expected||v[0]<1000||v[0]>1000000||v[1]<32768||v[1]>131072||
            (baseline_count && (u32)v[0]<=baseline[baseline_count-1].hz))return -1;
        x.hz=(u32)v[0];x.gain=v[1]/65536.0f;x.phase_deg=v[2]/1000.0f;x.flags=1;baseline[baseline_count++]=x;return 1;
    }
    if(!strcmp(name,"SWIMPORTEND")) {
        if(!end(p)||!importing||baseline_count!=import_expected||import_signature!=signature())return -1;
        importing=0;baseline_signature=signature();baseline_valid=1;return 1;
    }
    return 0;
}
void SweepCtrl_Poll(u32 now_ms)
{
    u32 w[24],status,gain; s32 bias;int result;
    now_r=now_ms;
    if(baseline_valid && baseline_signature!=signature())baseline_valid=0;
    if(pending_report){fill_summary();if(report(10,summary_words,16))pending_report=0;}
    if(role==ROLE_NONE)return;
    if(role==ROLE_MANUAL && state<STOP_DDS && DpllCtrl_IsRequestedOrActive())request_stop(1);
    switch(state) {
    case PROBE:
        if(read_reg(0)!=SWEEP_DEVICE_ID){device_ready=0;fault=1;fill_summary();report(10,summary_words,16);role=ROLE_NONE;state=IDLE;xil_printf("SWM1 required: download matching sweep bitstream\r\n");return;}
        device_ready=1;start_ms=now_ms;state=PREPARE;return;
    case PREPARE:
        if(!DdsCtrl_IsIdle())return;
        if(role!=ROLE_MANUAL && !prepared) {
            if(!queue_dds('S',0)||!queue_dds('W',1)||!queue_dds('P',0)||!queue_dds('E',1)){fault=2;request_stop(0);return;}
            prepared=1;return;
        }
        DdsCtrl_GetFeedback(&gain,&bias);
        if(role!=ROLE_MANUAL && bias && !bias_cleaned){DdsCtrl_SetOpenLoopOffset(0);bias_cleaned=1;return;}
        if(gain<1024){queue_dds('A',500);return;}
        if(role==ROLE_MANUAL && !DdsCtrl_IsFeedbackMode()){fault=3;request_stop(0);return;}
        state=CONFIGURE;return;
    case CONFIGURE:
        result=configure_measurement();if(!result)return;
        if(result<0){fault=4;request_stop(0);return;}state=WAIT_CONFIG;return;
    case WAIT_CONFIG:
        if(read_reg(REG_STATUS)&1)return;
        if(role==ROLE_MANUAL){expected_ftw=DdsCtrl_GetFtw();point_started=now_ms;state=MEASURE;}
        else state=SET_FREQUENCY;
        return;
    case SET_FREQUENCY:
        if(!DdsCtrl_IsIdle())return;
        gain=SweepMath_Frequency(stage_first,stage_last,point_index,point_total,phase_stage!=2);
        expected_ftw=ftw_from_hz(gain);if(!queue_dds('F',expected_ftw))return;
        state=WAIT_FREQUENCY;return;
    case WAIT_FREQUENCY:
        if(!DdsCtrl_IsIdle())return;
        point_started=now_ms;state=MEASURE;return;
    case MEASURE:
        if(role==ROLE_MANUAL && (manual_signature!=signature() || manual_channel!=(unsigned)AmplitudeLoop_Calibration()->channel_b ||
            last_manual_dds_gen!=DdsCtrl_GetGeneration())) {
            manual_channel=(unsigned)AmplitudeLoop_Calibration()->channel_b;state=CONFIGURE;return;
        }
        if(role==ROLE_MANUAL)expected_ftw=DdsCtrl_GetFtw();
        if(expected_ftw<ftw_from_hz(1000)||expected_ftw>ftw_from_hz(MAX_HZ)){fault=5;request_stop(0);return;}
        if((u32)(now_ms-point_started)>2000 && role!=ROLE_MANUAL){fault=6;request_stop(0);return;}
        if((s32)(now_ms-read_after_ms)<0)return;
        read_after_ms=now_ms+1; // bound AXI polling overhead without blocking Ethernet
        if(!snapshot(w)||w[0]==last_seq||w[2]!=expected_ftw||!(w[3]&1))return;
        last_seq=w[0];memcpy(last_words,w,sizeof(last_words));
        result=make_point(w);
        if(result<0){fault=7;request_stop(0);return;}
        if(!result)return;
        if(role==ROLE_MANUAL){point_started=now_ms;return;}
        state=EMIT;return;
    case EMIT:
        if(!report(9,emit_words,24))return;
        if(++point_index<point_total){state=SET_FREQUENCY;return;}
        state=ANALYZE;return;
    case ANALYZE:
        if(role==ROLE_BASE){baseline_signature=signature();baseline_target=settings.target_mv;baseline_valid=baseline_count==point_total;request_stop(0);return;}
        result=SweepMath_Find(curve,curve_count,(int)settings.mode,&features);
        if(result && fine_pass==0 && features.refine_high>features.refine_low){
            stage_first=features.refine_low;stage_last=features.refine_high;point_total=settings.fine;point_index=0;fine_pass=1;phase_stage=2;state=CONFIGURE;return;
        }
        if(result && settings.mode==SWEEP_BP && fine_pass==1 && (features.flags&3)==3 &&
           (stage_last-stage_first)/(float)(point_total-1)>features.bandwidth_hz/10.0f &&
           settings.coarse+2*settings.fine<=SWEEP_MAX_POINTS) {
            stage_first=features.refine_low;stage_last=features.refine_high;point_index=0;fine_pass=2;state=SET_FREQUENCY;return;
        }
        request_stop(0);return;
    case STOP_DDS:
        if(!DdsCtrl_IsIdle())return;
        if(!prepared) {
            if(stop_preserve){
                u32 ceiling,other;s32 center;
                DdsCtrl_GetFeedback(&gain,&bias);gain=zero_output?0:(last_words[17]?last_words[17]:gain);
                center=8388608+bias;ceiling=center<0?0:(u32)center/128U;
                other=center>16711680?0:(u32)(16711680-center)/127U;
                if(other<ceiling)ceiling=other;
                if(gain>ceiling)gain=ceiling;
                if(DdsCtrl_SetFeedback(gain,bias)!=DDS_CTRL_OK)return;
            }
            else if(role!=ROLE_MANUAL){if(!queue_dds('E',0))return;}
            prepared=1;return;
        }
        if(!DdsCtrl_IsIdle())return;
        state=STOP_PI;return;
    case STOP_PI:
        if(!DdsCtrl_IsIdle())return;
        status=device_ready?read_reg(REG_STATUS):0;if(status&1)return;
        if(prepared==1 && device_ready){write_reg(REG_CTRL,2);write_reg(REG_COMMIT,1);prepared=2;return;}
        fill_summary();if(!report(10,summary_words,16))return;
        xil_printf("Sweep done: fault=%u flags=0x%x points=%u cutoff=%u peak=%u Hz\r\n",
            (unsigned)fault,(unsigned)features.flags,(unsigned)curve_count,(unsigned)rounded(features.cutoff_hz),(unsigned)rounded(features.peak_hz));
        if(restore_channel>=0){AmplitudeLoop_Command(restore_channel?"FBCH1":"FBCH0");restore_channel=-1;}
        zero_output=0;role=ROLE_NONE;state=IDLE;return;
    case EXPORT_META:
        fill_meta();if(!report(11,meta_words,16))return;state=EXPORT_POINTS;return;
    case EXPORT_POINTS:
        if(export_index<baseline_count){
            w[0]=baseline[export_index].hz;w[1]=rounded(baseline[export_index].gain*65536);w[2]=(u32)signed_round(baseline[export_index].phase_deg*1000);
            if(report(12,w,3))++export_index;
            return;
        }
        state=EXPORT_END;return;
    case EXPORT_END:
        fill_summary();if(report(10,summary_words,16)){role=ROLE_NONE;state=IDLE;}return;
    default:return;
    }
}
void SweepCtrl_OverrideLoopStatus(u32 w[16])
{
    u32 f=last_words[16];float measured;
    if(!SweepCtrl_HardwarePiActive())return;
    if(last_words[1]!=expected_gen || state!=MEASURE) {
        w[1]=LOOP_WAIT_DATA;w[2]=(w[2]|3U)&~64U;
        w[3]=role==ROLE_MANUAL?manual_channel:0;
        w[4]=role==ROLE_MANUAL?manual_target:settings.target_mv;
        return;
    }
    w[1]=(f&16)?LOOP_ADC_CLIPPED:(f&8)?LOOP_BAD_SIGNAL:(f&4)?LOOP_LIMIT:(f&2)?LOOP_RUNNING:LOOP_WAIT_DATA;
    w[2]|=3U;w[3]=role==ROLE_MANUAL?manual_channel:0;w[4]=role==ROLE_MANUAL?manual_target:settings.target_mv;
    measured=last_words[w[3]?6:5]/65536.0f*slope(w[3],role!=ROLE_MANUAL);
    w[6]=rounded(measured);w[12]=last_words[17];
}
