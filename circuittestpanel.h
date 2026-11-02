#ifndef CIRCUITTESTPANEL_H
#define CIRCUITTESTPANEL_H

#include <QWidget>
#include <QGroupBox>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QTimer>
#include <QMap>
#include <QSet>
#include <QtEndian>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QScatterSeries>
#include <QtCharts/QLogValueAxis>
#include <QtCharts/QValueAxis>
#include <QtCharts/QLegend>
#include <functional>
#include <cmath>
#include <algorithm>
#include "experiment_analysis.h"

// The automatic measurement engine owns excitation, A-channel stabilisation
// and baseline correction. Do not infer transfer response from preview FFTs.
// Header-only so existing Qt Creator and verification targets share one UI.
class CircuitTestPanel : public QGroupBox
{
public:
    std::function<void(const QStringList &)> requestStart;
    std::function<void()> requestStop;
    std::function<void(bool)> requestManualVisibility;
    bool showManualControls() const { return m_manual->isChecked(); }
    ExperimentAnalysis::Sweep analysisSnapshot() const {
        auto data=m_analysis;data.status=m_analysis.status+"\n界面当前提示："+m_status->text();
        data.referenceNow=referenceMatches();
        for(const Point &p:m_points){ExperimentAnalysis::Point q;q.hz=p.hz;q.flags=p.flags;q.gain=p.gain;q.db=p.db;
            q.phase=p.phase;q.input=p.input;q.output=p.output;q.usable=p.usable;data.points.append(q);}
        return data;
    }

    explicit CircuitTestPanel(QWidget *parent = nullptr)
        : QGroupBox("电路响应测量", parent)
    {
        setObjectName("groupBox_circuitTest");
        auto layout = new QVBoxLayout(this);
        auto wiring = label("DAC → J2；J3（输入参考）→ ADC A；J4（电路输出）→ ADC B。先将 SW4 切到直通校正，再切到待测电路。", "label_circuitWiring");
        layout->addWidget(wiring);
        m_settings = new QWidget(this);
        auto form = new QGridLayout(m_settings); form->setContentsMargins(0,0,0,0);
        m_mode = new QComboBox(this); m_mode->setObjectName("comboBox_circuitMode");
        m_mode->addItems({"低通", "高通", "带通"});
        m_first = spin("spinBox_circuitFirst",1000,1000000,1000," Hz");
        m_last = spin("spinBox_circuitLast",1000,1000000,1000000," Hz");
        m_coarse = spin("spinBox_circuitCoarse",16,256,200,"");
        m_fine = spin("spinBox_circuitFine",16,256,101,"");
        m_target = new QDoubleSpinBox(this); m_target->setObjectName("spinBox_circuitTarget");
        m_target->setRange(0.1,5); m_target->setDecimals(3); m_target->setValue(1); m_target->setSuffix(" Vpp"); m_target->setKeyboardTracking(false);
        const QStringList titles{"电路类型","起始频率","终止频率","输入目标","粗扫点数","精扫点数"};
        QWidget *fields[] = {m_mode,m_first,m_last,m_target,m_coarse,m_fine};
        for(int i=0;i<6;++i) { form->addWidget(new QLabel(titles[i],this),(i/3)*2,i%3); form->addWidget(fields[i],(i/3)*2+1,i%3); form->setColumnStretch(i%3,1); }
        layout->addWidget(m_settings);
        auto actions = new QHBoxLayout;
        m_calibrate = new QPushButton("1. 直通校正",this); m_calibrate->setObjectName("btn_circuitCalibrate");
        m_start = new QPushButton("2. 开始测试",this); m_start->setObjectName("btn_circuitStart"); m_start->setProperty("primary",true);
        m_stop = new QPushButton("停止测试",this); m_stop->setObjectName("btn_circuitStop");
        actions->addWidget(m_calibrate); actions->addWidget(m_start); actions->addWidget(m_stop); layout->addLayout(actions);
        m_reference = label("直通参考：未校正", "label_circuitReference"); layout->addWidget(m_reference);
        m_status = label("连接设备后，先进行直通校正。自动测试使用正弦波、A 通道稳幅并清零数字偏置，结束后关闭测试输出。", "label_circuitStatus");
        layout->addWidget(m_status);
        auto results = new QGridLayout;
        const QStringList names{"截止频率 fc","谐振频率 f₀","下 −3 dB 点 fL","上 −3 dB 点 fH","带宽 BW","品质因数 Q"};
        const QStringList ids{"label_circuitCutoff","label_circuitResonance","label_circuitLow","label_circuitHigh","label_circuitBandwidth","label_circuitQ"};
        for(int i=0;i<6;++i) {
            auto box = new QWidget(this); auto card = new QVBoxLayout(box); card->setContentsMargins(8,5,8,5); card->setSpacing(2);
            auto title = new QLabel(names[i],box); title->setStyleSheet("color:#64748b;font-size:12px;"); card->addWidget(title);
            m_values[i] = new QLabel("—",box); m_values[i]->setObjectName(ids[i]); m_values[i]->setTextInteractionFlags(Qt::TextSelectableByMouse);
            m_values[i]->setStyleSheet("color:#164e90;font-size:19px;font-weight:bold;"); card->addWidget(m_values[i]);
            results->addWidget(box,i/3,i%3); results->setColumnStretch(i%3,1);
        }
        layout->addLayout(results);
        m_note = label("低通/高通：相对直通 0 dB 找 −3.01 dB；带通：相对响应峰值下降 3.01 dB。", "label_circuitResultNote"); layout->addWidget(m_note);
        m_pointLabel = label("测量点：等待板端数据", "label_circuitPoint"); layout->addWidget(m_pointLabel);
        auto tabs = new QTabWidget(this); tabs->setObjectName("tabs_circuitResponse");
        for(int i=0;i<2;++i) {
            m_charts[i] = new QtCharts::QChart; m_charts[i]->legend()->hide(); m_charts[i]->setAnimationOptions(QtCharts::QChart::NoAnimation);
            m_charts[i]->setMargins(QMargins(2,2,2,2));
            m_x[i] = new QtCharts::QLogValueAxis; m_x[i]->setBase(10); m_x[i]->setLabelFormat("%.0f"); m_x[i]->setTitleText("频率 (Hz，对数)"); m_x[i]->setRange(1000,1000000); m_x[i]->setMinorTickCount(8);
            m_y[i] = new QtCharts::QValueAxis; m_y[i]->setTitleText(i==0?"增益 B/A (dB)":"相位 B 相对 A (°)"); m_y[i]->setRange(i==0?-12:-180,i==0?3:180); m_y[i]->setLabelFormat("%.1f");
            m_charts[i]->addAxis(m_x[i],Qt::AlignBottom); m_charts[i]->addAxis(m_y[i],Qt::AlignLeft);
            auto view = new QtCharts::QChartView(m_charts[i],tabs); view->setObjectName(i==0?"chart_circuitGain":"chart_circuitPhase");
            view->setRenderHint(QPainter::Antialiasing); view->setMinimumSize(0,265); tabs->addTab(view,i==0?"幅频响应":"相频响应");
        }
        m_table = new QTableWidget(0,7,this); m_table->setObjectName("table_circuitPoints");
        m_table->setHorizontalHeaderLabels({"频率 Hz","增益 dB","幅比 B/A","相位 °","输入 Vpp","输出 Vpp","测量质量"});
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents); m_table->horizontalHeader()->setStretchLastSection(true); tabs->addTab(m_table,"测量数据");
        layout->addWidget(tabs);
        m_manual = new QPushButton("手动激励设置…",this); m_manual->setObjectName("btn_circuitManual");m_manual->setCheckable(true);
        m_manual->setToolTip("展开原有波形、频率、幅值和偏置设置。应用手动配置时会先结束正在进行的自动测试。");
        layout->addWidget(m_manual);
        connect(m_manual,&QPushButton::toggled,this,[this](bool show){if(requestManualVisibility)requestManualVisibility(show);});
        m_redraw = new QTimer(this); m_redraw->setSingleShot(true); m_redraw->setInterval(100);
        connect(m_redraw,&QTimer::timeout,this,[this](){ redraw(); });
        m_watchdog = new QTimer(this); m_watchdog->setSingleShot(true); m_watchdog->setInterval(15000);
        connect(m_watchdog,&QTimer::timeout,this,[this](){ m_status->setText("15 秒未收到新的测量数据，任务是否结束尚未确认。可点击停止测试；不会把旧结果当成本次结果。"); });
        connect(m_calibrate,&QPushButton::clicked,this,[this](){ queue(true); });
        connect(m_start,&QPushButton::clicked,this,[this](){ queue(false); });
        connect(m_stop,&QPushButton::clicked,this,[this](){ markStopping(); if(requestStop)requestStop(); });
        for(auto input : {m_first,m_last}) connect(input,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int){ refresh(); });
        connect(m_target,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this](double){ refresh(); });
        refresh();
    }

    void setConnectionState(bool connected, bool switching)
    {
        if(m_connected!=connected) {
            m_referenceValid=false;
            if(!connected) {
                if(m_active || m_waiting) m_status->setText("连接断开，本次测试未完成；板端可能仍在运行。重新连接后请核对设备，并重新直通校正。");
                else m_status->setText("连接断开；已显示结果仅为上次测量。重新连接后需重新直通校正。");
                m_active=m_waiting=m_stopping=false; m_analysis.running=false; m_watchdog->stop();
            }
            m_connected=connected;
        }
        m_switching=switching; refresh();
    }

    // Called only when the entire captured command batch has been queued.
    void outputDispatched(const QStringList &commands)
    {
        if(!m_waiting || (!commands.contains("SWCAL") && !commands.contains("SWSTART")))return;
        m_waiting=false; m_active=true; m_stopping=false; m_points.clear(); m_indices.clear(); m_problem=false;
        m_crossings.clear(); m_expectedTotal=0; m_signatureKnown=false; m_redraw->stop(); clearResults();
        m_analysis=ExperimentAnalysis::Sweep();m_analysis.started=QDateTime::currentDateTimeUtc();
        m_analysis.mode=m_taskMode;m_analysis.first=m_taskFirst;m_analysis.last=m_taskLast;m_analysis.targetMv=m_taskTarget;
        m_analysis.status="本次任务仍在运行，尚无最终结果";m_analysis.calibration=m_baseline;m_analysis.referenceAtStart=m_referenceValid;m_analysis.running=true;
        if(m_baseline)m_referenceValid=false;
        m_status->setText(m_baseline?"正在直通校正，等待测量点…":"正在测试，等待粗扫数据…");
        m_watchdog->start(); redraw(); refresh();
    }

    void outputChangeFailed(const QString &reason)
    {
        if(m_waiting) { m_waiting=false; m_stopping=false; m_status->setText("测试未启动："+reason); }
        else if(m_stopping && m_active) { m_stopping=false; m_status->setText("停止尚未确认："+reason+" 可重试停止。"); }
        refresh();
    }

    void markStopping()
    {
        if(m_active || m_waiting) { m_stopping=true; m_status->setText("正在停止测试，等待板端退出；本次不生成最终测量结果。"); refresh(); }
    }
    bool hasTask() const { return m_active || m_waiting; }

    void handleFrame(quint16 type, quint32 count, const QByteArray &payload)
    {
        if(!m_active)return;
        const auto u = [&payload](int i){return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(payload.constData())+i*4);};
        const auto s = [&payload](int i){return qFromLittleEndian<qint32>(reinterpret_cast<const uchar *>(payload.constData())+i*4);};
        if(type==9 && count==24 && payload.size()==96 && u(0)==1) {
            if((m_baseline?u(1)!=0:(u(1)!=1 && u(1)!=2)) || u(16)!=quint32(m_taskMode))return;
            if(u(3)!=quint32(u(1)==2?m_taskFine:m_taskCoarse) || u(2)>=u(3) || u(4)<quint32(m_taskFirst) || u(4)>quint32(m_taskLast)) { m_problem=true; return; }
            if(m_signatureKnown && m_signature!=u(17)) { m_problem=true; return; }
            if(!m_baseline && m_referenceSignature!=u(17)) { m_referenceValid=false; m_problem=true; return; }
            m_signature=u(17); m_signatureKnown=true;
            Point point; point.hz=u(4); point.flags=u(5); point.gain=u(6)/65536.0; point.db=s(7)/1000.0; point.phase=s(8)/1000.0; point.input=u(9)/1000.0; point.output=u(10)/1000.0;
            point.usable=(point.flags&1) && !(point.flags&12) && (m_baseline || (point.flags&2));
            if(m_points.size()>=512 && !m_points.contains(point.hz)) {m_problem=true;return;}
            m_points.insert(point.hz,point); m_indices.insert((u(1)<<16)|u(2));
            if(m_baseline)m_expectedTotal=int(u(3));
            m_pointLabel->setText(pointText(point));
            if(!m_stopping) m_status->setText(QString("%1：第 %2 / %3 点 · 已接收 %4 个不同频率")
                .arg(u(1)==0?"直通校正":u(1)==1?"粗扫":"精扫").arg(u(2)+1).arg(u(3)).arg(m_points.size()));
            m_watchdog->start(); if(!m_redraw->isActive())m_redraw->start(); return;
        }
        if(type!=10 || count!=16 || payload.size()!=64 || u(0)!=1 || u(1)!=(m_baseline?0U:2U) || u(2)!=quint32(m_taskMode))return;
        // SWSTATUS shares this frame type. This panel never polls SWSTATUS;
        // only a locally submitted task can consume its automatic completion.
        m_active=false; m_watchdog->stop(); m_redraw->stop();
        m_analysis.running=false;m_analysis.finished=QDateTime::currentDateTimeUtc();
        for(int i=0;i<16;++i)m_analysis.summary.append(u(i));
        const quint32 features=u(3), fault=u(13);
        if(m_stopping) {
            m_stopping=false; m_status->setText("测试已停止；曲线为本次已收到的部分数据，不作为完整测试结果。");
            if(m_baseline)m_referenceValid=false;
        } else if(fault || (features&0x80000000U)) {
            m_status->setText("测试失败："+faultText(fault)+"。本次不显示截止频率等最终结果。");
            if(m_baseline || fault==7)m_referenceValid=false;
        } else if(m_problem || !m_signatureKnown || u(14)!=quint32(m_taskFirst) || u(15)!=quint32(m_taskLast)
                  || u(4)!=quint32(m_points.size()) || m_points.isEmpty()) {
            m_status->setText("结果不完整或参数不一致，不能确认本次测量；请重新校正/测试。");
            if(m_baseline)m_referenceValid=false;
        } else if(m_baseline) {
            bool allValid=m_expectedTotal==m_taskCoarse && m_indices.size()==m_taskCoarse && m_points.size()==m_taskCoarse;
            for(const Point &p : m_points)allValid=allValid && p.usable;
            m_referenceValid=allValid;
            if(allValid) {
                m_analysis.accepted=true;
                m_referenceFirst=m_taskFirst; m_referenceLast=m_taskLast; m_referenceTarget=m_taskTarget; m_referenceSignature=m_signature;
                m_status->setText("直通校正完成。请将 SW4 切到待测电路，选择类型后点击开始测试。");
            } else m_status->setText("直通校正数据有缺失、弱信号或削顶；请检查接线和输入目标后重新校正。");
        } else {
            int usable=0; for(const Point &p:m_points)if(p.usable)++usable;
            bool stagesComplete=true;
            for(int i=0;i<m_taskCoarse;++i)stagesComplete=stagesComplete && m_indices.contains((1U<<16)|quint32(i));
            bool hasFine=false;for(quint32 index:m_indices)hasFine=hasFine || (index>>16)==2;
            if(hasFine)for(int i=0;i<m_taskFine;++i)stagesComplete=stagesComplete && m_indices.contains((2U<<16)|quint32(i));
            if(!stagesComplete) m_status->setText("粗扫或精扫点有缺失，无法确认本次完整结果；请重新测试。");
            else if(usable<3) m_status->setText("有效测量点不足；请检查接线、输入目标和削顶/弱信号标记后重测。");
            else {
                m_analysis.accepted=true;
                auto inRange=[this](quint32 hz){return hz>=quint32(m_taskFirst) && hz<=quint32(m_taskLast);};
                bool badFeature=false;
                auto frequency = [this,&badFeature,&inRange](quint32 hz, bool found) {
                    if(!found || !inRange(hz))return QString("未找到");
                    if(!supportedFrequency(hz)){badFeature=true;return QString("无法确认");}
                    return formatHz(hz);
                };
                bool incompleteBandpass=false;
                if(m_taskMode==0 || m_taskMode==1) {
                    const bool found=features&(m_taskMode==0?2U:1U);
                    m_values[0]->setText(frequency(u(6),found));
                    // Firmware fills cutoff_hz for LP/HP; low_hz/high_hz are BP-only.
                    m_values[m_taskMode==0?3:2]->setText(frequency(u(6),found));
                    if(found && inRange(u(6)) && supportedFrequency(u(6))){m_crossings.append(u(6));
                        m_analysis.confirmed[0]=u(6);m_analysis.confirmed[m_taskMode==0?3:2]=u(6);}
                } else {
                    const QString resonance=frequency(u(7),features&4);
                    m_values[2]->setText(frequency(u(8),features&1)); m_values[3]->setText(frequency(u(9),features&2));
                    if((features&15)==7 && u(8)<u(7) && u(7)<u(9) && inRange(u(8)) && inRange(u(9)) && inRange(u(7))
                        && supportedFrequency(u(8)) && supportedFrequency(u(9)) && supportedFrequency(u(7))
                        && u(10)>0 && u(11)>0 && std::abs(double(u(10))-double(u(9)-u(8)))<=1) {
                        m_values[1]->setText(resonance);
                        m_values[4]->setText(formatHz(u(10))); m_values[5]->setText(QString::number(u(11)/65536.0,'f',3));
                        m_crossings.append(u(8));m_crossings.append(u(9));
                        m_analysis.confirmed[1]=u(7);m_analysis.confirmed[2]=u(8);m_analysis.confirmed[3]=u(9);
                        m_analysis.confirmed[4]=u(10);m_analysis.confirmed[5]=u(11)/65536.0;
                    } else {
                        incompleteBandpass=true;
                        m_values[1]->setText("未确认");m_values[4]->setText("未找到");m_values[5]->setText("未找到");
                        const QString candidate=(features&4) && u(7)>0 ? "板端候选峰值："+formatHz(u(7))+"。" : QString();
                        const QString explanation=candidate+"未获得完整有效的两侧 −3 dB 交点及带宽，不能确认为带通谐振。已显示的交点仅相对候选峰值。";
                        m_values[1]->setToolTip(explanation);
                        m_note->setText(m_note->text()+"\n"+explanation);
                    }
                }
                bool missing=(features&8) || badFeature || incompleteBandpass;
                for(auto value:m_values)missing=missing || value->text()=="未找到";
                if(m_taskMode==2)missing=missing || (features&7)!=7;
                QString advice="；测试输出已关闭。";
                if(badFeature)advice="；特征附近有弱信号、削顶或无效点，请检查信号后重测。";
                else if(missing) {
                    const bool needsLower=m_taskMode!=0 && !(features&1);
                    const bool needsUpper=m_taskMode!=1 && !(features&2);
                    if((needsLower && m_taskFirst>1000) || (needsUpper && m_taskLast<1000000))
                        advice="；可在支持范围 1 kHz～1 MHz 内扩大扫描范围，再按需重新校正、测试。";
                    else if(needsLower || needsUpper)
                        advice="；已到缺失交点方向的支持范围边界（1 kHz～1 MHz），请核对电路、接线和测量曲线。";
                    else advice="；特征不完整或数据不一致，请核对电路、接线和测量数据后复测。";
                }
                m_status->setText(QString("%1 · %2 · %3 个频率点 · 耗时 %4 s%5")
                    .arg(incompleteBandpass?"测试结束，带通未确认":missing?"测试结束，特征未找齐":"测试完成").arg(QStringList{"低通","高通","带通"}[m_taskMode]).arg(m_points.size()).arg(u(5)/1000.0,0,'f',2)
                    .arg(advice));
                if(usable<m_points.size())m_status->setText(m_status->text()+QString("\n已略去 %1 个不合格测量点，详见数据表；仅显示有有效相邻数据支持的特征。").arg(m_points.size()-usable));
                if(u(12)>0 && !missing)m_note->setText(m_note->text()+QString("\n内置测试板模型：电容估算 %1 nF（%2）；其他电路不适用此估算。").arg(u(12)/1000.0,0,'f',3).arg(m_taskMode==2?"按 L=3.3 mH":"按 R=1.6 kΩ"));
            }
        }
        m_analysis.status=m_status->text();redraw(); refresh();
    }

private:
    ExperimentAnalysis::Sweep m_analysis;
    struct Point { quint32 hz=0,flags=0; double gain=0,db=0,phase=0,input=0,output=0; bool usable=false; };
    QLabel *label(const QString &text,const char *name) {auto p=new QLabel(text,this); p->setObjectName(name); p->setWordWrap(true); p->setTextInteractionFlags(Qt::TextSelectableByMouse); return p;}
    QSpinBox *spin(const char *name,int low,int high,int value,const QString &suffix) {auto p=new QSpinBox(this);p->setObjectName(name);p->setRange(low,high);p->setValue(value);p->setSuffix(suffix);p->setKeyboardTracking(false);return p;}
    static QString formatHz(double hz) {return hz>=1000000?QString::number(hz/1000000,'f',3)+" MHz":hz>=1000?QString::number(hz/1000,'f',3)+" kHz":QString::number(hz,'f',0)+" Hz";}
    static QString quality(const Point &p) {return p.usable?"有效":(p.flags&8)?"削顶":(p.flags&4)?"弱信号":"无效/未校正";}
    static QString pointText(const Point &p) {return QString("%1 · 增益 %2 dB（×%3）· 相位 %4°\n输入 A：%5 Vpp / 输出 B：%6 Vpp · %7")
        .arg(formatHz(p.hz)).arg(p.db,0,'f',3).arg(p.gain,0,'f',4).arg(p.phase,0,'f',3).arg(p.input,0,'f',3).arg(p.output,0,'f',3).arg(quality(p));}
    static QString faultText(quint32 code) {const QStringList text{"未说明的板端故障","位流 ID 不匹配","DDS 队列失败","输出模式不正确","电压标定或输入目标不可用","频率不支持","输入稳幅超时","直通参考或插值失败"};return code<quint32(text.size())?text[int(code)]:QString("未知故障 %1").arg(code);}
    bool referenceMatches() const {return m_referenceValid && m_first->value()>=m_referenceFirst && m_last->value()<=m_referenceLast && qRound(m_target->value()*1000)==m_referenceTarget;}
    bool supportedFrequency(quint32 hz) const
    {
        auto right=m_points.lowerBound(hz);
        if(right==m_points.cend())return false;
        if(right.key()==hz)return right.value().usable;
        if(right==m_points.cbegin() || !right.value().usable)return false;
        --right;return right.value().usable;
    }
    void refresh()
    {
        const bool busy=m_active || m_waiting;
        m_settings->setEnabled(m_connected && !busy && !m_switching);
        m_calibrate->setEnabled(m_connected && !busy && !m_switching);
        m_start->setEnabled(m_connected && !busy && !m_switching && referenceMatches());
        m_stop->setEnabled(m_connected && busy && !m_stopping);
        m_reference->setText(referenceMatches()?QString("直通参考：有效 · %1～%2 · 输入目标 %3 Vpp").arg(formatHz(m_referenceFirst),formatHz(m_referenceLast)).arg(m_referenceTarget/1000.0,0,'f',3)
            :m_referenceValid?"直通参考：当前频率范围或输入目标不匹配，需重新校正":"直通参考：未校正或已失效；接线、电压标定改变后也需重新校正");
    }
    void queue(bool baseline)
    {
        for(auto input:{m_first,m_last,m_coarse,m_fine})if(!input->hasAcceptableInput()) {m_status->setText("未发送：请填写有效参数。");return;}
        if(!m_target->hasAcceptableInput() || m_first->value()>=m_last->value() || m_coarse->value()+m_fine->value()>512) {m_status->setText("未发送：起始频率须小于终止频率，粗扫与精扫合计不超过 512 点。");return;}
        if(!baseline && !referenceMatches())return;
        m_baseline=baseline; m_taskMode=m_mode->currentIndex(); m_taskFirst=m_first->value(); m_taskLast=m_last->value(); m_taskCoarse=m_coarse->value(); m_taskFine=m_fine->value(); m_taskTarget=qRound(m_target->value()*1000);
        m_waiting=true; m_stopping=false;
        m_status->setText("正在准备测试；如有已知活动模式，会先自动退出。请保持当前接线/开关位置。"); refresh();
        const QString config=QString("SWCONFIG %1 %2 %3 %4 %5 %6").arg(m_taskFirst).arg(m_taskLast).arg(m_taskCoarse).arg(m_fine->value()).arg(m_taskTarget).arg(m_taskMode);
        if(requestStart)requestStart({"LOOP0","E0","S0",config,baseline?"SWCAL":"SWSTART"});
        else outputChangeFailed("测量控制未连接。");
    }
    void clearResults()
    {
        for(auto p:m_values){p->setText("—");p->setToolTip(QString());}
        m_note->setText("低通/高通：相对直通 0 dB 找 −3.01 dB；带通：相对响应峰值下降 3.01 dB。橙色虚线为参考线，红点为找到的交点。");
        m_pointLabel->setText("测量点：等待板端数据");
    }
    void redraw()
    {
        double peak=-1e9;
        for(const Point &p:m_points)if(p.usable)peak=std::max(peak,p.db);
        const bool thresholdVisible=!m_baseline && peak>-1e8;
        const double threshold=m_taskMode==2?peak-3.01029995664:-3.01029995664;
        for(int c=0;c<2;++c) {
            auto chart=m_charts[c]; chart->removeAllSeries(); m_x[c]->setRange(m_taskFirst,m_taskLast);
            chart->setTitle(m_baseline?(c==0?"直通幅频参考":"直通相位参考"):(c==0?"电路幅频响应（已直通校正）":"电路相频响应（已直通校正）"));
            double low=1e9,high=-1e9; QtCharts::QLineSeries *line=nullptr; double previous=0; bool havePrevious=false;
            auto dots=new QtCharts::QScatterSeries; dots->setObjectName(c==0?"series_circuitGainPoints":"series_circuitPhasePoints"); dots->setMarkerSize(4); dots->setColor(QColor("#2869d8")); dots->setBorderColor(QColor("#2869d8"));
            for(const Point &p:m_points) {
                if(!p.usable) {line=nullptr;havePrevious=false;continue;}
                const double v=c==0?p.db:p.phase; low=std::min(low,v); high=std::max(high,v);
                if(!line || (c==1 && havePrevious && std::abs(v-previous)>=180)) {line=new QtCharts::QLineSeries; line->setPen(QPen(QColor("#2869d8"),1.5)); chart->addSeries(line); line->attachAxis(m_x[c]); line->attachAxis(m_y[c]);}
                line->append(p.hz,v); dots->append(p.hz,v); previous=v;havePrevious=true;
            }
            chart->addSeries(dots); dots->attachAxis(m_x[c]); dots->attachAxis(m_y[c]);
            connect(dots,&QtCharts::QScatterSeries::hovered,this,[this](QPointF p,bool hovered){if(hovered && m_points.contains(quint32(p.x())))m_pointLabel->setText(pointText(m_points.value(quint32(p.x()))));});
            if(c==0 && thresholdVisible) {
                auto ref=new QtCharts::QLineSeries; ref->setObjectName("series_circuitThreshold"); ref->setPen(QPen(QColor("#d97706"),1,Qt::DashLine)); ref->append(m_taskFirst,threshold);ref->append(m_taskLast,threshold);chart->addSeries(ref);ref->attachAxis(m_x[c]);ref->attachAxis(m_y[c]);
                low=std::min(low,threshold);high=std::max(high,threshold);
                if(!m_crossings.isEmpty()) {
                    auto markers=new QtCharts::QScatterSeries; markers->setObjectName("series_circuitCrossings");
                    markers->setMarkerSize(10); markers->setColor(QColor("#dc2626")); markers->setBorderColor(Qt::white);
                    for(quint32 hz:m_crossings)markers->append(hz,threshold);
                    chart->addSeries(markers);markers->attachAxis(m_x[c]);markers->attachAxis(m_y[c]);
                    connect(markers,&QtCharts::QScatterSeries::hovered,this,[this](QPointF p,bool hovered){if(hovered)m_pointLabel->setText("−3 dB 交点："+formatHz(p.x()));});
                }
            }
            if(low<=high){const double margin=std::max(c==0?1.0:5.0,(high-low)*0.1);m_y[c]->setRange(low-margin,high+margin);}
            else m_y[c]->setRange(c==0?-12:-180,c==0?3:180);
        }
        m_table->setRowCount(m_points.size()); int row=0;
        for(const Point &p:m_points) {
            const QStringList cells{QString::number(p.hz),QString::number(p.db,'f',3),QString::number(p.gain,'f',5),QString::number(p.phase,'f',3),QString::number(p.input,'f',3),QString::number(p.output,'f',3),quality(p)};
            for(int col=0;col<cells.size();++col)m_table->setItem(row,col,new QTableWidgetItem(cells[col])); ++row;
        }
    }

    QWidget *m_settings;
    QSpinBox *m_first,*m_last,*m_coarse,*m_fine;
    QDoubleSpinBox *m_target;
    QComboBox *m_mode;
    QPushButton *m_calibrate,*m_start,*m_stop,*m_manual;
    QLabel *m_reference,*m_status,*m_pointLabel,*m_note,*m_values[6];
    QtCharts::QChart *m_charts[2]; QtCharts::QLogValueAxis *m_x[2]; QtCharts::QValueAxis *m_y[2];
    QTableWidget *m_table; QTimer *m_redraw,*m_watchdog;
    QMap<quint32,Point> m_points; QSet<quint32> m_indices;
    QVector<quint32> m_crossings;
    bool m_connected=false,m_switching=false,m_active=false,m_waiting=false,m_stopping=false,m_baseline=true,m_referenceValid=false,m_signatureKnown=false,m_problem=false;
    int m_taskMode=0,m_taskFirst=1000,m_taskLast=1000000,m_taskCoarse=200,m_taskFine=101,m_taskTarget=1000,m_expectedTotal=0;
    int m_referenceFirst=0,m_referenceLast=0,m_referenceTarget=0;
    quint32 m_signature=0,m_referenceSignature=0;
};
#endif
