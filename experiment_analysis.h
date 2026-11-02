#ifndef EXPERIMENT_ANALYSIS_H
#define EXPERIMENT_ANALYSIS_H
#include <QVector>
#include <QByteArray>
#include <QStringList>
#include <QDateTime>
#include <QElapsedTimer>
#include <QtEndian>
#include <QPointF>
#include <array>
#include <cmath>
#include <limits>
#include <algorithm>

namespace ExperimentAnalysis {
inline double unknown() { return std::numeric_limits<double>::quiet_NaN(); }
inline bool positive(double x) { return std::isfinite(x) && x>0; }
inline QString number(double x,const QString &unit=QString()) {return std::isfinite(x)?QString::number(x,'g',8)+unit:QString("未确认");}
inline quint32 word(const QByteArray &p,int i) {return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(p.constData())+4*i);}
inline qint32 signedWord(const QByteArray &p,int i) {return qFromLittleEndian<qint32>(reinterpret_cast<const uchar *>(p.constData())+4*i);}
struct Point {double hz=0,db=0,phase=0,gain=0,input=0,output=0;quint32 flags=0;bool usable=false;};
struct Sweep {
    QVector<Point> points;
    std::array<double,6> confirmed{{unknown(),unknown(),unknown(),unknown(),unknown(),unknown()}}; // fc,f0,fL,fH,BW,Q
    QVector<quint32> summary;
    QDateTime started,finished;
    int mode=0,first=1000,last=1000000,targetMv=1000;
    bool calibration=false,referenceAtStart=false,referenceNow=false,running=false,accepted=false;
    QString status="尚未进行电路测量";
};
struct ProcessPoint {qint64 ms=0;int segment=0;double reference=0,measured=0,error=0,phase=0,gain=0;bool locked=false,limited=false;};
struct Process {
    QVector<ProcessPoint> pll,loop;
    QStringList events;
    QString pllState="尚无 PLL 状态回复",loopState="尚无稳幅状态回复";
    qint64 pllStart=-1,loopStart=-1,firstLock=-1,lastPllReply=-1,lastLoopReply=-1,now=0;
    int pllInvalid=0,loopInvalid=0,lockLosses=0;
    bool connected=false,pllTrimmed=false,loopTrimmed=false;
};
struct Snapshot {Sweep sweep;Process process;QDateTime captured=QDateTime::currentDateTimeUtc();QString context;};

class History {
public:
    History() {m_clock.start();}
    void connection(bool connected) {
        if(connected){m_data=Process();m_clock.restart();m_havePllKey=false;m_lastLocked=false;m_expectedTarget=0;m_pllSegment=m_loopSegment=0;m_haveGeneration=false;}
        m_data.connected=connected;
        event(connected?"TCP 已连接；旧会话记录已清除":"TCP 断开；保留历史，不能据此判断板端当前状态");
    }
    void event(const QString &text) {
        m_data.events<<QString("%1 s：%2").arg(m_clock.elapsed()/1000.0,0,'f',3).arg(text.left(400));
        while(m_data.events.size()>80)m_data.events.removeFirst();
    }
    void commands(const QStringList &commands) {
        QStringList actions;
        for(const QString &c:commands) {
            if(c.endsWith("STATUS"))continue;
            actions<<c;
            if(c=="PLL1") {m_data.lastPllReply=-1;m_haveGeneration=false;++m_pllSegment;m_data.pll.clear();m_data.pllStart=m_clock.elapsed();m_data.firstLock=-1;m_data.lockLosses=0;m_data.pllInvalid=0;m_data.pllTrimmed=false;m_havePllKey=false;m_lastLocked=false;m_data.pllState="PLL 启用请求已排队；执行和锁定未确认";}
            if(c=="PLL0")m_data.pllState="PLL 停用请求已排队；历史回复不代表当前状态";
            if((c.startsWith("VPP") && c!="VPP0") || c=="LOOP1") {
                m_data.lastLoopReply=-1;++m_loopSegment;m_data.loop.clear();m_data.loopStart=m_clock.elapsed();m_data.loopInvalid=0;m_data.loopTrimmed=false;
                if(c.startsWith("VPP"))m_expectedTarget=c.mid(3).toUInt();
                m_data.loopState="稳幅启动请求已排队；尚未确认达到目标";
            }
            if(c=="LOOP0" || c=="VPP0")m_data.loopState="稳幅停止请求已排队；历史回复不代表当前状态";
        }
        if(!actions.isEmpty())event("请求已排队（执行未确认）："+actions.join(" → "));
    }
    void pll(quint32 flags,const QByteArray &p) {
        if(p.size()!=64)return;
        const quint32 result=word(p,2);const qint64 now=m_clock.elapsed();m_data.lastPllReply=now;
        const bool valid=(flags&4) && !(flags&0xC0000001U) && !(flags&~0xC000000FU) && !(result&~0x7FU);
        const bool comparable=valid && (flags&2) && (result&3)==3 && !(result&0x78) && word(p,4)>0;
        const bool locked=comparable && (result&4) && (flags&8);
        const QString key=QString("%1/%2/%3/%4").arg(word(p,0)).arg(word(p,1)).arg(flags).arg(result);
        if(m_havePllKey && m_pllKey==key)return;
        m_pllKey=key;m_havePllKey=true;
        if(!valid)m_data.pllState=(flags&0x80000000U)?"PLL 硬件 ID 不匹配":"PLL 快照无效、正在提交或存在未知标志";
        else if(!(result&1))m_data.pllState="最近有效回复：PLL 已停用";
        else if(result&8)m_data.pllState="最近回复：ADC 参考削顶";
        else if(result&16)m_data.pllState="最近回复：参考超时或丢失";
        else if(result&32)m_data.pllState="最近回复：参考周期超范围";
        else if(result&64)m_data.pllState="最近回复：采样间断标记";
        else m_data.pllState=locked?"最近有效回复：已观察到锁定":comparable?"最近有效回复：正在跟踪，未确认锁定":"最近回复：等待有效参考";
        if(m_lastLocked && valid && (result&1) && !locked){++m_data.lockLosses;event("观察到 PLL 锁定丢失；"+m_data.pllState);}
        if(valid)m_lastLocked=locked;
        if(!comparable){++m_data.pllInvalid;++m_pllSegment;return;}
        if(m_haveGeneration && m_generation!=word(p,1)){m_data.pll.clear();m_data.pllStart=m_data.firstLock=-1;m_data.pllTrimmed=false;++m_pllSegment;event("PLL 配置代号变化；重新记录，不能沿用前次启动时间");}
        m_generation=word(p,1);m_haveGeneration=true;
        ProcessPoint point;point.ms=now;point.segment=m_pllSegment;point.reference=65000000.0*65536/word(p,4);point.measured=65000000.0*word(p,3)/4294967296.0;
        point.error=point.measured-point.reference;point.phase=signedWord(p,5)*360.0/4294967296.0;point.locked=locked;
        if(locked && m_data.firstLock<0 && m_data.pllStart>=0)m_data.firstLock=now;
        append(m_data.pll,point,m_data.pllTrimmed);
    }
    void loop(const QByteArray &p,bool manual) {
        if(p.size()!=64 || word(p,0)!=1 || !manual)return;
        const qint64 now=m_clock.elapsed();m_data.lastLoopReply=now;
        const quint32 state=word(p,1),flags=word(p,2);
        const bool usable=(state==6 || state==7) && (flags&0x6B)==0x6B && !(flags&0x80) && !(flags&~0xFFFU)
            && !(flags&4U) && word(p,4)>0 && word(p,3)<=1 && word(p,15)<=1 && (!m_expectedTarget || word(p,4)==m_expectedTarget);
        m_data.loopState=usable?(state==7?"最近回复：输出限幅，目标未必达到":"最近回复：稳幅运行，是否达标需比较误差")
            :QString("最近回复：稳幅状态 %1；数值不作为当前有效测量").arg(state);
        if(!usable){++m_data.loopInvalid;++m_loopSegment;return;}
        ProcessPoint point;point.ms=now;point.segment=m_loopSegment;point.reference=word(p,4)/1000.0;point.measured=word(p,6)/1000.0;
        point.error=(point.measured-point.reference)/point.reference*100;point.gain=word(p,12)/65536.0;point.limited=state==7 || (flags&0x400);
        append(m_data.loop,point,m_data.loopTrimmed);
    }
    Process snapshot() const {Process p=m_data;p.now=m_clock.elapsed();return p;}
private:
    static void append(QVector<ProcessPoint> &v,const ProcessPoint &p,bool &trimmed) {if(v.size()>=1200){v.removeFirst();trimmed=true;}v.append(p);}
    Process m_data;QElapsedTimer m_clock;QString m_pllKey;bool m_havePllKey=false,m_lastLocked=false;quint32 m_expectedTarget=0,m_generation=0;bool m_haveGeneration=false;int m_pllSegment=0,m_loopSegment=0;
};

enum Model { NoModel, RcLowPass, RcHighPass, SeriesRlcBandPass };
struct Theory {
    int model=NoModel;double r=0,c=0,l=0; // SI units; user-supplied nominal values, never inferred from fc.
    bool valid() const {return model>NoModel && model<=SeriesRlcBandPass && positive(r) && positive(c) && (model!=SeriesRlcBandPass || positive(l));}
    std::array<double,6> features() const {
        std::array<double,6> f{{unknown(),unknown(),unknown(),unknown(),unknown(),unknown()}};
        if(!valid())return f;
        const double pi=std::acos(-1.0);
        if(model==RcLowPass || model==RcHighPass){f[0]=1/(2*pi*r*c);f[model==RcLowPass?3:2]=f[0];}
        else {const double d=std::hypot(r,2*std::sqrt(l/c));f[1]=1/(2*pi*std::sqrt(l*c));f[3]=(d+r)/(4*pi*l);f[2]=1/(4*pi*pi*l*c*f[3]);f[4]=r/(2*pi*l);f[5]=f[1]/f[4];}
        return f;
    }
    QPointF response(double hz) const {
        if(!valid() || !positive(hz))return QPointF(unknown(),unknown());
        const double pi=std::acos(-1.0),w=2*pi*hz;
        double gain,phase;
        if(model==RcLowPass){const double x=w*r*c;gain=1/std::hypot(1.0,x);phase=-std::atan(x);}
        else if(model==RcHighPass){const double x=w*r*c;gain=x/std::hypot(1.0,x);phase=std::atan(1/x);}
        else {const double x=w*l-1/(w*c);gain=r/std::hypot(r,x);phase=-std::atan2(x,r);}
        return QPointF(20*std::log10(gain),phase*180/pi);
    }
};

inline QString sweepReport(const Sweep &s,const Theory &theory) {
    QString text="【电路测量事实】\n"+s.status+"\n";
    text+=QString("来源：%1；模式：%2；范围：%3～%4 Hz；输入目标：%5 Vpp；频率点：%6。\n")
        .arg(s.calibration?"直通校正（不是待测电路响应）":"板端 type9/type10 扫频")
        .arg(QStringList{"低通","高通","带通"}.value(s.mode,"未知")).arg(s.first).arg(s.last).arg(s.targetMv/1000.0).arg(s.points.size());
    text+="测量开始："+(s.started.isValid()?s.started.toLocalTime().toString(Qt::ISODate):"未知")+"；结束："+(s.finished.isValid()?s.finished.toLocalTime().toString(Qt::ISODate):"尚未结束")+"。\n";
    text+=QString("开始时直通参考：%1；当前参考：%2。\n").arg(s.referenceAtStart?"有效":"未确认",s.referenceNow?"有效":"未校正或已失效");
    int valid=0,weak=0,clip=0;double maxDb=-1e30,maxHz=0;
    for(const Point &p:s.points){if(p.usable){++valid;if(p.db>maxDb){maxDb=p.db;maxHz=p.hz;}}if(p.flags&4)++weak;if(p.flags&8)++clip;}
    text+=QString("有效点 %1；不合格点 %2（弱信号 %3、削顶 %4，类别可重叠）。\n").arg(valid).arg(s.points.size()-valid).arg(weak).arg(clip);
    if(valid)text+=QString("有效点中的最大增益 %1 dB，位置 %2 Hz；这是离散扫描最大点，不能直接等同谐振。\n").arg(maxDb).arg(maxHz);
    const QStringList names{"截止频率 fc (Hz)","谐振频率 f0 (Hz)","下交点 fL (Hz)","上交点 fH (Hz)","带宽 BW (Hz)","Q"};
    text+="已确认的测量特征：\n";for(int i=0;i<6;++i)text+="- "+names[i]+"："+number(s.confirmed[i])+"\n";
    if(s.summary.size()==16)text+=QString("板端完成帧：stage=%1，特征 flags=0x%2，fault=%3，候选峰值=%4 Hz。缺失交点或失败时，不将候选峰值确认为谐振。\n")
        .arg(s.summary[1]).arg(s.summary[3],8,16,QLatin1Char('0')).arg(s.summary[13]).arg(s.summary[7]);
    text+="【本地异常检查与验证建议】\n";
    if(s.points.isEmpty())text+="- 没有扫频点，不能判断滤波器类型；先确认完成直通校正并启动电路测试。\n";
    if(s.calibration)text+="- 本次是直通参考，不能据此确认待测滤波电路；切到实际支路后测量。\n";
    if(s.running)text+="- 任务仍在进行，当前曲线是部分数据，不作最终截止/带宽结论。\n";
    if(!s.accepted && !s.calibration)text+="- 结果未通过完整性检查或任务失败；保留部分曲线用于排查，不能当作完整实验结果。\n";
    if(clip)text+="- 有削顶标记：核对 ADC 输入及输出余量，适当降低目标后重新直通校正；不能仅凭标记确定哪个模拟器件故障。\n";
    if(weak)text+="- 有弱信号点：核对其频率位置、实际输出幅值和测量动态范围，不把低信噪比相位当作可靠结果。\n";
    if(s.summary.size()==16 && s.summary[13]==6)text+="- 板端故障6：单点约2秒未取得合格测量；可能包括输入未稳到目标、快照/频率/状态条件未满足。检查 A 实际幅值、调节余量和板端状态；最后成功点不是故障频率。\n";
    if(s.mode==2 && !std::isfinite(s.confirmed[1]) && !s.calibration)text+="- 带通未确认：需在同一峰值两侧取得有效 −3 dB 交点。高频平台上的小波动可能成为候选峰值；核对实际带通支路及输出取点。\n";
    if(s.first==1000 && s.last==1000000)text+="- 已覆盖现有板端支持的1 kHz～1 MHz，不能继续越界扩大扫描。\n";
    if(valid>=2){const Point *first=nullptr,*last=nullptr;for(const Point &p:s.points)if(p.usable){if(!first)first=&p;last=&p;}
        if(first && last && s.mode==2 && last->db>first->db+3 && std::abs(last->db)<1.5)
            text+="- 低频较弱、高频接近0 dB，当前更像高通或未覆盖上侧衰减；这只是曲线依据，不能据此断言开关拨错或电路损坏。\n";
    }
    text+="【理论与实测对照】\n";
    if(!theory.valid())text+="未指定完整的理论模型和实际标称元件值，不能计算理论偏差；不默认套用1.6 kΩ或3.3 mH。\n";
    else {
        text+=QString("用户指定模型=%1；R=%2 Ω；C=%3 F；L=%4 H。\n").arg(theory.model).arg(theory.r,0,'g',9).arg(theory.c,0,'g',9).arg(theory.l,0,'g',9);
        text+="模型假设：RC为一阶、通带增益1；RLC为串联RLC且输出取有效串联电阻两端，R包含有效串联损耗。其他拓扑不能直接套用。\n";
        const int expected=theory.model==RcLowPass?0:theory.model==RcHighPass?1:2;
        if(s.mode!=expected || s.calibration)text+="所选理论模型与本次实测类型/数据来源不匹配，不计算偏差。\n";
        const auto predicted=theory.features();
        for(int i=0;i<6;++i)if(positive(predicted[i])){
            text+="- "+names[i]+"：理论 "+number(predicted[i]);
            if(s.mode==expected && !s.calibration && positive(s.confirmed[i]))text+="；实测 "+number(s.confirmed[i])+"；相对偏差 "+number((s.confirmed[i]/predicted[i]-1)*100," %");
            else text+="；实测未确认或不适用";
            text+="\n";
        }
        if(s.accepted && !s.calibration && s.mode==expected){double db2=0,phase2=0;int n=0;for(const Point &p:s.points)if(p.usable){const auto v=theory.response(p.hz);const double dp=std::remainder(p.phase-v.y(),360.0);db2+=(p.db-v.x())*(p.db-v.x());phase2+=dp*dp;++n;}
            if(n)text+=QString("全扫描有效点的模型差异 RMS：增益 %1 dB，相位 %2°（跨全范围、不是仪器精度或噪声指标）。\n").arg(std::sqrt(db2/n)).arg(std::sqrt(phase2/n));}
        text+="未提供元件容差和验收门限，仅报告偏差，不判定合格/不合格，不把元件估算值当成独立实测标称值。\n";
    }
    return text;
}

inline QString processReport(const Process &p,double bandPercent=2) {
    QString text="【PLL / 幅值闭环过程事实】\n";
    text+=p.connected?"TCP 已连接；以下为收到的状态记录，非连续硬件波形。\n":"TCP 未连接；以下仅是保留的历史记录，不能判断板端当前状态。\n";
    text+="PLL："+p.pllState+"；有效记录 "+QString::number(p.pll.size())+"，未纳入数值趋势的回复（含停用/参考异常） "+QString::number(p.pllInvalid)+"。\n";
    if(p.lastPllReply>=0)text+="最近 PLL 回复距快照 "+number((p.now-p.lastPllReply)/1000.0," s")+"。\n";
    if(!p.pll.isEmpty()){
        const auto last=p.pll.last();double sum=0,phase2=0,maxError=0;
        for(const auto &v:p.pll){sum+=v.error;phase2+=v.phase*v.phase;maxError=std::max(maxError,std::abs(v.error));}
        text+=QString("最近有效 PLL 点：参考 %1 Hz；输出 %2 Hz；频差（输出−参考）%3 Hz；相位误差 %4°。\n有效记录频差均值 %5 Hz，最大绝对频差 %6 Hz，相位误差 RMS %7°。\n")
            .arg(last.reference).arg(last.measured).arg(last.error).arg(last.phase).arg(sum/p.pll.size()).arg(maxError).arg(std::sqrt(phase2/p.pll.size()));
    }
    text+=p.firstLock>=0 && p.pllStart>=0?"从启动请求到首次观察到有效锁定："+number((p.firstLock-p.pllStart)/1000.0," s")+"（含轮询和网络延迟，非精确硬件锁定时间）。\n":"未观察到可关联本次启动请求的有效锁定，不能给出锁定时间。\n";
    text+="观察到的锁定丢失次数："+QString::number(p.lockLosses)+"；采样间隔内未观测到的变化无法统计。\n";
    text+="稳幅："+p.loopState+"；有效记录 "+QString::number(p.loop.size())+"，无效/状态不满足回复 "+QString::number(p.loopInvalid)+"。\n";
    if(p.lastLoopReply>=0)text+="最近稳幅回复距快照 "+number((p.now-p.lastLoopReply)/1000.0," s")+"。\n";
    if(!p.loop.isEmpty()){
        const auto last=p.loop.last();double sum=0,squares=0,overshoot=0;int limited=0; qint64 reached=-1;
        for(int i=0;i<p.loop.size();++i){const auto &v=p.loop[i];sum+=v.error;squares+=v.error*v.error;overshoot=std::max(overshoot,v.error);if(v.limited)++limited;
            if(reached<0 && i>0 && v.segment==p.loop[i-1].segment && v.ms-p.loop[i-1].ms<=2000 && std::abs(v.error)<=bandPercent && std::abs(p.loop[i-1].error)<=bandPercent)reached=v.ms;}
        text+=QString("最近有效稳幅点：目标 %1 Vpp，实测 %2 Vpp，误差 %3 %，DAC gain %4。\n有效回复误差均值 %5 %，误差 RMS %6 %，已观测最大正向超调 %7 %，限幅记录 %8。\n")
            .arg(last.reference).arg(last.measured).arg(last.error).arg(last.gain).arg(sum/p.loop.size()).arg(std::sqrt(squares/p.loop.size())).arg(overshoot).arg(limited);
        text+=QString("达标观察条件：连续两次有效状态回复（中间无异常、间隔不超过2秒）都在 ±%1 % 内。\n").arg(bandPercent);
        if(reached>=0 && p.loopStart>=0)text+=(p.loopTrimmed?QString("保留窗口内首次连续达标距启动请求："):QString("从启动请求到首次观察到连续达标："))+number((reached-p.loopStart)/1000.0," s")+"；不代表此后始终稳定，实际过渡可能发生在两次回复之间。\n";
        else text+="尚未观察到可关联启动请求的连续达标，不能给出收敛时间。\n";
    }
    if(p.pll.size()<3 || p.loop.size()<3)text+="对应过程少于3个有效点时，不判断稳定性、振荡或漂移趋势。\n";
    if(p.pllTrimmed || p.loopTrimmed)text+="过程超过1200点，仅保留最新窗口；汇总极值不是整段实验极值。稳幅更早过程已丢弃，不能推断整次首次达标。\n";
    text+="PLL 采用测量序号/配置代号去重；无效快照、参考丢失及未知标志不进入数值趋势。稳幅回复没有测量时间戳/序号，不能据此给出精确高频抖动。\n";
    text+="【排查与下一步】\nPLL确认ADC A外部参考、频率范围及削顶标记；参考异常时先恢复有效参考再判断锁定。稳幅确认实际反馈通道、目标与输出余量。过程点不足时启用对应的状态自动查询后重新记录。\n";
    text+="最近操作/状态事件（请求排队不等于板端执行）：\n"+p.events.join("\n")+"\n";
    return text;
}
inline QString aiEvidence(const Snapshot &s,const Theory &theory,int mode,double bandPercent) {
    QString out=QString("数据快照时间：%1；入口：%2。\n").arg(s.captured.toLocalTime().toString(Qt::ISODate),s.context);
    if(mode!=3)out+=sweepReport(s.sweep,theory);
    if(mode>=2)out+=processReport(s.process,bandPercent);
    if(mode!=3){out+="【按频率排序的测量点：Hz,增益dB,相位deg,输入Vpp,输出Vpp,flags,可用】\n";
        for(const auto &p:s.sweep.points)out+=QString("%1,%2,%3,%4,%5,%6,%7\n").arg(p.hz).arg(p.db).arg(p.phase).arg(p.input).arg(p.output).arg(p.flags).arg(p.usable?1:0);}
    if(mode>=2){out+="【过程明细：最近最多120个有效状态点；时间为距连接建立的毫秒；segment变化表示中间存在异常回复】\nPLL:ms,fref_Hz,fout_Hz,df_Hz,phase_deg,locked,segment\n";
        for(int i=std::max(0,s.process.pll.size()-120);i<s.process.pll.size();++i){const auto &p=s.process.pll[i];out+=QString("%1,%2,%3,%4,%5,%6,%7\n").arg(p.ms).arg(p.reference).arg(p.measured).arg(p.error).arg(p.phase).arg(p.locked?1:0).arg(p.segment);}
        out+="LOOP:ms,target_Vpp,measured_Vpp,error_pct,gain,limited,segment\n";
        for(int i=std::max(0,s.process.loop.size()-120);i<s.process.loop.size();++i){const auto &p=s.process.loop[i];out+=QString("%1,%2,%3,%4,%5,%6,%7\n").arg(p.ms).arg(p.reference).arg(p.measured).arg(p.error).arg(p.gain).arg(p.limited?1:0).arg(p.segment);}}
    return out;
}
}
#endif
