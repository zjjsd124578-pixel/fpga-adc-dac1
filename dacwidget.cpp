#include "dacwidget.h"
#include "circuittestpanel.h"
#include "ui_dacwidget.h"
#include <QTcpSocket>
#include <QTimer>
#include <QHostAddress>
#include "dds_frequency.h"
#include "app_style.h"
#include "adcwidget.h"
#include <QCheckBox>
#include <QtEndian>
#include <QLabel>
#include <QGroupBox>
#include <QFormLayout>
#include <QPlainTextEdit>
#include <cmath>

namespace {
// Board-measured DC correction. Keep this out of the user interface and settings.
constexpr double DacOffsetCalibrationCoefficient = 0.5;
}

DacWidget::DacWidget(QWidget *parent, AdcWidget *controller)
    : QWidget(parent), ui(new Ui::DacWidget),
      m_socket(controller ? controller->sharedSocket() : new QTcpSocket(this)),
      m_controller(controller), m_connectTimer(new QTimer(this)),
      m_receiveTimer(new QTimer(this))
{
    ui->setupUi(this);
    m_loopRecording=new QCheckBox("记录调节过程（每 0.5 秒查询）",this);
    m_loopRecording->setObjectName("checkBox_loopProcessRecording");
    m_loopRecording->setToolTip("建议启动稳幅前勾选，再在 AI 实验分析中更新快照。仅记录状态，不改变目标或输出；关闭后仍可手动查询。");
    ui->amplitudeLoopPanelLayout->insertWidget(2,m_loopRecording);
    m_loopRefreshTimer=new QTimer(this);m_loopRefreshTimer->setInterval(500);
    m_loopReplyTimer=new QTimer(this);m_loopReplyTimer->setInterval(3000);m_loopReplyTimer->setSingleShot(true);
    connect(m_loopRefreshTimer,&QTimer::timeout,this,&DacWidget::pollLoopProcess);
    connect(m_loopRecording,&QCheckBox::toggled,this,[this](bool enabled){
        if(enabled)m_loopRefreshTimer->start();else m_loopRefreshTimer->stop();
    });
    connect(m_loopReplyTimer,&QTimer::timeout,this,[this](){
        m_loopQueryPending=false;m_loopRecording->setChecked(false);
        m_analysisHistory.event("稳幅过程查询超时，已停止自动查询；板端输出未被停止");
        ui->label_loopStatus->setText("3 秒未收到稳幅状态，过程查询已暂停；板端输出状态未知。可重新勾选记录。");
    });
    createPllControls();
    m_circuitPanel = new CircuitTestPanel(this);
    ui->dacContentLayout->insertWidget(1, m_circuitPanel);
    m_circuitPanel->hide();
    m_circuitPanel->requestManualVisibility = [this](bool show) {
        ui->groupBox->setVisible(m_circuitTestMode && show);
    };
    m_circuitPanel->requestStart = [this](const QStringList &commands) {
        beginOutputChange(commands, "电路测试请求已发送，等待板端测量数据");
    };
    m_circuitPanel->requestStop = [this]() {
        if (m_modeStep != ModeStep::Idle) {
            cancelOutputChange("已取消尚未下发的电路测试请求。");
            return;
        }
        beginOutputChange({}, "电路测试已退出", false);
    };
    m_modePollTimer = new QTimer(this);
    m_modePollTimer->setInterval(200);
    m_modeDeadline = new QTimer(this);
    m_modeDeadline->setSingleShot(true);
    m_modeDeadline->setInterval(6000);
    m_modeStatus = new QLabel("点击下发配置即可发送；查询状态不作为普通输出的前置条件", this);
    m_modeStatus->setObjectName("label_modeSwitchStatus");
    m_modeStatus->setWordWrap(true);
    ui->logLayout->insertWidget(0, m_modeStatus);
    connect(m_modePollTimer, &QTimer::timeout, this, &DacWidget::pollModeStatus);
    connect(m_modeDeadline, &QTimer::timeout, this, &DacWidget::modeConfirmationTimedOut);
    ui->btn_connect->setProperty("primary", true);
    ui->btn_sendWave->setProperty("primary", true);
    ui->btn_startSweep->setProperty("primary", true);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(signalAnalyzerStyle() + "QWidget#dacScrollBody { background: #f3f6fa; }");
    ui->textEdit_log->setMaximumBlockCount(500);
    ui->comboBox_wave->addItem("正弦波", 1);
    ui->comboBox_wave->addItem("余弦波", 2);
    ui->comboBox_wave->addItem("方波", 3);
    ui->comboBox_wave->addItem("三角波", 4);
    ui->comboBox_wave->addItem("锯齿波", 5);
    ui->comboBox_feedbackChannel->addItem("A 通道", 0);
    ui->comboBox_feedbackChannel->addItem("B 通道", 1);
    ui->comboBox_feedbackChannel->setCurrentIndex(1);
    ui->comboBox_amplitudeMode->addItem("幅值比例（%）", 0);
    ui->comboBox_amplitudeMode->addItem("峰峰值（Vpp，自动稳幅）", 1);
    connect(ui->comboBox_amplitudeMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this](int) { refreshAmplitudeMode(); });
    ui->comboBox_feedbackChannel->setToolTip("选择实际接入的 ADC 反馈通道，下发配置时发送 FBCH0/FBCH1。只切换选项不会发送；恢复闭环沿用板端已有通道和目标。");
    ui->btn_resumeLoop->setToolTip("发送 LOOP1，恢复板端已有通道和目标；修改界面参数后请点击下发配置。");
    for (QDoubleSpinBox *input : {ui->spinBox_freq,
        ui->spinBox_startFreq_2, ui->spinBox_stopFreq, ui->doubleSpinBox_5}) {
        input->setDecimals(0);
        input->setRange(0, 4294967295.0);
    }
    for (QDoubleSpinBox *input : {ui->spinBox_freq, ui->spinBox_startFreq_2,
        ui->spinBox_stopFreq, ui->doubleSpinBox_5}) {
        input->setDecimals(3);
        input->setMaximum(500000000);
        input->setSuffix(" Hz");
    }
    ui->spinBox_phase->setDecimals(3);
    ui->spinBox_phase->setRange(0.0, 360.0);
    ui->spinBox_phase->setSingleStep(1.0);
    ui->spinBox_phase->setSuffix(" °");
    ui->spinBox_phase->setToolTip("输入 0～360°，下发时换算为 32 位相位字；360° 与 0° 等效。修改输入不会自动下发。");
    ui->label_phase->setText("相位偏移");
    ui->label_sweep->setText("扫频步进");
    ui->spinBox_freq->setValue(10000);
    ui->spinBox_startFreq_2->setValue(1000);
    ui->spinBox_stopFreq->setValue(2000);
    ui->doubleSpinBox_5->setValue(50);
    ui->groupBox->setToolTip("频率输入 Hz，按 32 位累加器和实际 DDS 时钟换算 FTW；相位输入角度，自动换算为 32 位相位字。");
    ui->groupBox_2->setToolTip("起始频率、结束频率和步进均输入 Hz，自动换算 FTW；波形和幅值采用固定波形区的设置。本功能不生成测量曲线。");
    ui->btn_stopSweep->setText("停止扫频并关闭输出");
    ui->btn_stopSweep->setToolTip("发送 E0、S0，关闭固定输出和扫频。当前协议不能读取并锁存扫频瞬间的频率，停止不宣称保持该频率。");
    setAmplitudeLoopMode(false);
    m_connectTimer->setSingleShot(true);
    m_connectTimer->setInterval(10000);
    m_receiveTimer->setInterval(1000);
    connect(m_receiveTimer, &QTimer::timeout, this, [this]() {
        updateReceiveStatistics();
        if (ui->checkBox_receiveHex->isChecked() && m_hasNewData) {
            ui->textEdit_log->appendPlainText("接收 HEX 摘要（最新数据块前 32 字节）："
                + QString::fromLatin1(m_hexPreview.toHex(' ')));
        }
        m_hasNewData = false;
    });
    connect(ui->btn_back, &QPushButton::clicked, this, &DacWidget::requestBack);
    connect(ui->btn_connect, &QPushButton::clicked, this, &DacWidget::toggleConnection);
    connect(ui->btn_sendWave, &QPushButton::clicked, this, &DacWidget::sendFixedWave);
    connect(ui->btn_stopWave, &QPushButton::clicked, this, [this]() {
        beginOutputChange({"LOOP0", "E0", "S0"}, "关闭固定输出和扫频命令已发送，执行未确认");
    });
    connect(ui->btn_stopLoop, &QPushButton::clicked, this, [this]() {
        beginOutputChange({"LOOP0"}, "停止稳幅请求已发送；按板端逻辑保留增益", false);
    });
    connect(ui->btn_resumeLoop, &QPushButton::clicked, this, [this]() {
        beginOutputChange({"LOOP1"}, "已发送恢复请求，闭环运行未确认");
    });
    connect(ui->btn_loopStatus, &QPushButton::clicked, this, [this]() {
        if(m_loopQueryPending)return;
        if (sendCommands({"LOOPSTATUS"})) {

            ui->label_loopStatus->setText("已查询，等待设备回复");
        }
    });
    connect(ui->btn_startSweep, &QPushButton::clicked, this, &DacWidget::startSweep);
    connect(ui->btn_stopSweep, &QPushButton::clicked, this, [this]() {
        beginOutputChange({"LOOP0", "E0", "S0"}, "已发送停止 DDS 扫频并关闭输出请求");
    });
    if (m_controller) {
        connect(m_controller, &AdcWidget::sharedConnectionChanged, this, [this]() {
            ui->lineEdit_ip->setText(m_controller->connectionIp());
            ui->spinBox_port->setValue(m_controller->connectionPort());
            refreshConnectionState();
        });
        connect(m_controller, &AdcWidget::tcpBytesReceived, this, &DacWidget::observeReceivedBytes);
        connect(m_controller, &AdcWidget::closedLoopStatusReceived, this, &DacWidget::showLoopStatus);
        connect(m_controller, &AdcWidget::protocolStatusFrameReceived, this, &DacWidget::handleModeStatus);
        ui->groupBox_connection->setTitle("设备连接（ADC / DAC 共用 TCP）");
        ui->btn_connect->setToolTip("两页共用连接；断开会同时停止 ADC / DAC 通信，返回或切换页面不会断开。");
    }
    connect(m_socket, &QTcpSocket::stateChanged, this, [this](QAbstractSocket::SocketState state) {
        const bool idle = state == QAbstractSocket::UnconnectedState;
        const bool connected = state == QAbstractSocket::ConnectedState;
        refreshConnectionState();
        if (idle || connected) m_connectTimer->stop();
        if (idle) m_receiveTimer->stop();
    });
    connect(m_socket, &QTcpSocket::connected, this, [this]() {
        m_analysisHistory.connection(true);
        m_autoTaskActive = false;
        m_manualLoopPossible = false;
        m_pllMayBeActive = false;
        m_expectedTaskStage = -1;

        m_statusBuffer.clear();
        m_modeStatus->setText("已连接；点击下发配置即可发送");
        m_pllStatus->setText("已连接，尚未查询数字 PLL 状态");
        refreshConnectionState();
        m_receivedBytes = 0;
        m_receivedChunks = 0;
        m_hexPreview.clear();
        m_hasNewData = false;
        updateReceiveStatistics();
        m_receiveTimer->start();
        ui->textEdit_log->appendPlainText(QString("TCP 已连接 %1:%2，请选择固定波形或扫频模式。不会自动恢复输出。")
            .arg(m_socket->peerAddress().toString()).arg(m_socket->peerPort()));
    });
    connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
        m_analysisHistory.connection(false);m_loopRecording->setChecked(false);
        m_loopReplyTimer->stop();m_loopQueryPending=false;
        cancelOutputChange("连接断开，已取消待发配置；重新连接不会自动恢复。");
        m_autoTaskActive = false;
        m_statusBuffer.clear();
        m_pllAutoQuery->setChecked(false);
        m_pllStatus->setText("连接断开，PLL 状态未知；板端可能仍在运行");
        m_pllDetails->clear();
        ui->label_loopStatus->setText("连接断开，板端闭环状态未知");
        updateReceiveStatistics();
        ui->textEdit_log->appendPlainText("TCP 连接已断开。");
    });
    connect(m_socket, &QTcpSocket::readyRead, this, [this]() {
        // The shared stream has one reader: ADC frame assembly. DAC observes a copy.
        if (!m_controller) {
            const QByteArray bytes = m_socket->readAll();
            observeReceivedBytes(bytes);
            parseStandaloneFrames(bytes);
        }
    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_socket, &QAbstractSocket::errorOccurred, this,
#else
    connect(m_socket, static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(&QAbstractSocket::error), this,
#endif
        [this](QAbstractSocket::SocketError) {
            ui->textEdit_log->appendPlainText("TCP 错误：" + m_socket->errorString());
            m_connectTimer->stop();
            if (!m_controller) m_socket->abort();
        });
    connect(m_connectTimer, &QTimer::timeout, this, [this]() {
        ui->textEdit_log->appendPlainText("TCP 连接超时（10 秒），请检查 FPGA IP、监听端口和网线。");
        m_socket->abort();
    });
    if(m_socket->state()==QAbstractSocket::ConnectedState)m_analysisHistory.connection(true);
    if(m_controller){QPointer<DacWidget> source(this);m_controller->setExperimentAnalysisSource([source](){
        return source?source->experimentSnapshot():ExperimentAnalysis::Snapshot();
    });}
    refreshConnectionState();
}

ExperimentAnalysis::Snapshot DacWidget::experimentSnapshot() const
{
    ExperimentAnalysis::Snapshot data;data.sweep=m_circuitPanel->analysisSnapshot();data.process=m_analysisHistory.snapshot();
    data.context=m_circuitTestMode?"待测电路测试":m_digitalPllMode?"数字 PLL":(m_amplitudeLoopMode || usesTargetAmplitude())?"幅值闭环":"DAC 信号发生";
    return data;
}

void DacWidget::pollLoopProcess()
{
    if(!m_loopRecording->isChecked() || m_loopQueryPending || m_digitalPllMode || m_autoTaskActive
        || m_modeStep!=ModeStep::Idle || !m_manualLoopPossible || !m_socket || m_socket->state()!=QAbstractSocket::ConnectedState)return;
    if(sendCommands({"LOOPSTATUS"},false)){m_loopQueryPending=true;m_loopReplyTimer->start();}
}

void DacWidget::createPllControls()
{
    m_pllPanel = new QGroupBox("数字 PLL · ADC A 参考", this);
    m_pllPanel->setObjectName("groupBox_digitalPll");
    auto layout = new QVBoxLayout(m_pllPanel);
    auto hint = new QLabel("外部参考接 ADC A，DAC 跟随参考频率与相位；ADC B 可观察输出。默认验证范围 1 kHz～100 kHz。输出沿用已有直流偏置；失参考可能静音。", m_pllPanel);
    hint->setWordWrap(true); layout->addWidget(hint);
    m_pllSettings = new QGroupBox("PLL 参数", m_pllPanel);
    auto form = new QFormLayout(m_pllSettings);
    auto spin = [this, form](const char *name, const QString &label, int low, int high, int value) {
        auto input = new QSpinBox(m_pllSettings); input->setObjectName(name);
        input->setRange(low, high); input->setValue(value); form->addRow(label, input); return input;
    };
    m_pllMinHz = spin("spinBox_pllMinHz", "最低参考频率", 992, 253906, 1000);
    m_pllMaxHz = spin("spinBox_pllMaxHz", "最高参考频率", 992, 253906, 100000);
    m_pllMinHz->setSuffix(" Hz"); m_pllMaxHz->setSuffix(" Hz");
    m_pllCenter = spin("spinBox_pllCenter", "过零中心码", 0, 4095, 2048);
    m_pllCenter->setToolTip("规范化偏移二进制中心码；即使选择补码，零电平中心也通常填 2048。");
    m_pllHysteresis = spin("spinBox_pllHysteresis", "过零滞回（码）", 1, 1023, 8);
    m_pllKp = spin("spinBox_pllKp", "KP 右移位数", 1, 12, 2);
    m_pllKi = spin("spinBox_pllKi", "KI 右移位数", 0, 20, 6);
    m_pllKp->setToolTip("参数是右移位数，不是直接的 Kp；KI 必须至少比 KP 大 2。");
    m_pllKi->setToolTip(m_pllKp->toolTip());
    m_pllFormat = new QComboBox(m_pllSettings); m_pllFormat->setObjectName("comboBox_pllFormat");
    m_pllFormat->addItem("偏移二进制", 0); m_pllFormat->addItem("二进制补码", 1);
    form->addRow("ADC A 原始编码", m_pllFormat);
    m_pllAmplitude = new QDoubleSpinBox(m_pllSettings);
    m_pllAmplitude->setObjectName("spinBox_pllAmplitude"); m_pllAmplitude->setRange(0,100);
    m_pllAmplitude->setDecimals(1); m_pllAmplitude->setSuffix(" %"); m_pllAmplitude->setValue(50);
    form->addRow("DAC 手动幅值比例", m_pllAmplitude);
    m_pllColumns = new QBoxLayout(QBoxLayout::LeftToRight);
    m_pllColumns->setSpacing(16);
    m_pllColumns->addWidget(m_pllSettings,1);
    auto results = new QWidget(m_pllPanel);
    auto resultLayout = new QVBoxLayout(results); resultLayout->setContentsMargins(0,0,0,0);
    m_pllColumns->addWidget(results,1); layout->addLayout(m_pllColumns);
    auto actions = new QHBoxLayout;
    auto button = [this, actions](const char *name, const QString &text) {
        auto b = new QPushButton(text, m_pllPanel); b->setObjectName(name); actions->addWidget(b); return b;
    };
    m_pllStart = button("btn_pllStart", "配置并启动 PLL"); m_pllStart->setProperty("primary", true);
    m_pllStop = button("btn_pllStop", "停止 PLL");
    m_pllStop->setToolTip("直接发送 PLL0，后台查询停用状态；不等同关闭 DAC 输出。");
    m_pllQuery = button("btn_pllStatus", "刷新一次");
    m_pllQuery->setToolTip("手动刷新当前状态。实时更新会在后台进行，不需要反复点击。");
    m_pllClose = button("btn_pllCloseOutput", "关闭输出");
    resultLayout->addLayout(actions);
    m_pllAutoQuery = new QCheckBox("实时更新状态（每 0.5 秒）", m_pllPanel);
    m_pllAutoQuery->setObjectName("checkBox_pllAutoQuery"); resultLayout->addWidget(m_pllAutoQuery);
    m_pllStatus = new QLabel("尚未查询；打开页面不会自动启用 PLL", m_pllPanel);
    m_pllStatus->setObjectName("label_pllStatus"); m_pllStatus->setWordWrap(true); resultLayout->addWidget(m_pllStatus);
    m_pllDetails = new QPlainTextEdit(m_pllPanel); m_pllDetails->setObjectName("textEdit_pllDetails");
    m_pllDetails->setReadOnly(true); m_pllDetails->setMinimumHeight(150); resultLayout->addWidget(m_pllDetails,1);
    ui->dacContentLayout->insertWidget(1, m_pllPanel); m_pllPanel->hide();
    m_pllRefreshTimer = new QTimer(this); m_pllRefreshTimer->setInterval(500);
    m_pllReplyTimer = new QTimer(this); m_pllReplyTimer->setSingleShot(true); m_pllReplyTimer->setInterval(3000);
    connect(m_pllStart, &QPushButton::clicked, this, &DacWidget::startDigitalPll);
    connect(m_pllQuery, &QPushButton::clicked, this, [this]() { queryDigitalPll(); });
    connect(m_pllStop, &QPushButton::clicked, this, [this]() {
        m_pllAutoQuery->setChecked(false);
        // An explicit stop is always sent, even if no PLL status has arrived.
        m_pendingOutput.clear();
        m_pendingResult = "PLL 停用状态已收到；DAC 是否继续输出取决于原输出设置";
        m_stopPll = true;
        m_pllMayBeActive = true;
        beginPllStop();
    });
    connect(m_pllClose, &QPushButton::clicked, this, [this]() {
        m_pllAutoQuery->setChecked(false);
        beginOutputChange({"LOOP0", "E0", "S0"}, "关闭 DAC 输出请求已发送，执行未确认");
    });
    connect(m_pllAutoQuery, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled && m_digitalPllMode) m_pllRefreshTimer->start(); else m_pllRefreshTimer->stop();
    });
    connect(m_pllRefreshTimer, &QTimer::timeout, this, [this]() { queryDigitalPll(false); });
    connect(m_pllReplyTimer, &QTimer::timeout, this, [this]() {
        m_pllQueryPending = false; m_pllAutoQuery->setChecked(false);
        m_pllStatus->setText("PLL 查询超时，不能用旧结果判断当前锁定状态");
        ui->textEdit_log->appendPlainText("PLLSTATUS 查询 3 秒未回复；可再次查询，普通 DAC 下发不受此查询影响。");
        refreshConnectionState();
    });
}

void DacWidget::setDigitalPllMode(bool enabled)
{
    m_digitalPllMode = enabled;
    m_pllPanel->setVisible(enabled);
    ui->groupBox->setVisible(!enabled);
    if (enabled) ui->groupBox_2->hide();
    ui->label_dacTitle->setText(enabled ? "数字 PLL" : "DAC 信号发生");
    ui->label_ddsClock->setVisible(!enabled); ui->spinBox_ddsClock->setVisible(!enabled); ui->label_ddsBits->setVisible(!enabled);
    if (enabled) ui->label_protocolNotice->setText("PLL 按板端固定 65 MHz 采样时钟换算；KP/KI 为右移位数。参数编辑不会自动下发，启动后查询实际状态。");
    else setAmplitudeLoopMode(m_amplitudeLoopMode);
    if (!enabled) m_pllAutoQuery->setChecked(false);
}

void DacWidget::startDigitalPll()
{
    for (auto input : {m_pllMinHz,m_pllMaxHz,m_pllCenter,m_pllHysteresis,m_pllKp,m_pllKi}) {
        if (!input->hasAcceptableInput()) { m_pllStatus->setText("未发送：请填写有效 PLL 参数"); return; }
    }
    const quint32 minHz = quint32(m_pllMinHz->value()), maxHz = quint32(m_pllMaxHz->value());
    const quint32 longest = quint32((65000000ULL + minHz/2) / minHz);
    const quint32 shortest = quint32((65000000ULL + maxHz/2) / maxHz);
    if (minHz > maxHz || longest > 65534 || shortest < 256 || m_pllKi->value() < m_pllKp->value()+2
        || !m_pllAmplitude->hasAcceptableInput() || m_pllFormat->currentIndex() < 0) {
        m_pllStatus->setText("未发送：最低频率不能大于最高频率，周期须在 256～65534，KI 必须 ≥ KP+2。"); return;
    }
    if (minHz < 1000 || maxHz > 100000)
        ui->textEdit_log->appendPlainText("PLL 频率超出已验证的 1 kHz～100 kHz；满足命令范围不代表实机已验证。");
    QStringList commands{"LOOP0", "E0", "S0", "W1", "A"+QString::number(qRound(m_pllAmplitude->value()*10)), "P0", "E1"};
    // Firmware validates each field against its current range/KP/KI. Widen
    // intermediate limits first so crossing the previous range cannot reject
    // an otherwise valid complete configuration.
    commands << "PLLFORMAT"+QString::number(m_pllFormat->currentData().toInt())
        << "PLLCENTER"+QString::number(m_pllCenter->value())
        << "PLLHYST"+QString::number(m_pllHysteresis->value())
        << "PLLMAXHZ253906" << "PLLMINHZ"+QString::number(minHz) << "PLLMAXHZ"+QString::number(maxHz)
        << "PLLKI20" << "PLLKP"+QString::number(m_pllKp->value()) << "PLLKI"+QString::number(m_pllKi->value()) << "PLL1";
    beginOutputChange(commands, "PLL 启用请求已发送，等待查询实际状态；不代表已锁定");
}

void DacWidget::queryDigitalPll(bool logRequest)
{
    if (!m_socket || m_socket->state()!=QAbstractSocket::ConnectedState || m_modeStep!=ModeStep::Idle) return;
    if (m_pllQueryPending) {
        // A manual click can share an outstanding background request. Keep the
        // one-request rule: replies have no request ID, and mode exit uses it too.
        if (logRequest && !m_pllManualQuery) {
            m_pllManualQuery = true;
            m_pllStatus->setText("正在刷新状态，等待本次回复；旧结果不代表当前锁定状态");
            m_pllStatus->setStyleSheet("color:#9a5b12;");
            refreshConnectionState();
        }
        return;
    }
    if (sendCommands({"PLLSTATUS"}, logRequest)) {
        m_pllQueryPending = true; m_pllManualQuery = logRequest;
        m_pllQueryObsolete = false; m_pllReplyTimer->start();
        if (logRequest) {
            m_pllStatus->setText("已查询 PLL，等待回复；旧结果不代表当前锁定状态");
            m_pllStatus->setStyleSheet("color:#9a5b12;");
        }
        refreshConnectionState();
    }
}

void DacWidget::showPllStatus(quint32 flags, const QByteArray &payload)
{
    if (payload.size()!=64) return;
    m_analysisHistory.pll(flags,payload);
    const auto u = [&payload](int i) { return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(payload.constData())+4*i); };
    const auto s = [&payload](int i) { return qFromLittleEndian<qint32>(reinterpret_cast<const uchar *>(payload.constData())+4*i); };
    const quint32 result = u(2);
    const bool valid = (flags & 6U)==6U && !(flags & 0xC0000001U);
    const bool known = !(flags & ~0xC000000FU) && !(result & ~0x7FU);
    QString state;
    if (flags & 0x80000000U) state = "PLL 硬件 ID 不匹配，请核实 DPL1 位流";
    else if (flags & 0x40000001U) state = "配置正在提交／应用，不能判断当前锁定";
    else if (!valid) state = "尚无有效 PLL 快照，不能判断启用或锁定";
    else if (!known) state = "含未定义状态位，不能判断锁定";
    else if (!(result & 1U)) state = "PLL 已停用（最近查询结果）";
    else if (result & 8U) state = "PLL 已启用，ADC 参考削顶";
    else if (result & 16U) state = "PLL 已启用，参考超时／丢失，DAC 可能静音";
    else if (result & 32U) state = "PLL 已启用，参考周期超出范围";
    else if (result & 64U) state = "PLL 已启用，检测到采样间断标记（bit6）";
    else if ((result & 7U)==7U && (flags & 8U) && u(4)) state = "PLL 已锁定（最近有效快照）";
    else if (result & 2U) state = "PLL 正在跟踪，尚未确认锁定";
    else state = "PLL 已启用，等待有效参考";
    m_pllStatus->setText(state);
    const double reference = u(4) ? 65000000.0*65536.0/u(4) : 0;
    m_pllDetails->setPlainText(QString("测量序号：%1；配置代号：%2\n帧头 flags：0x%3；结果 flags：0x%4\n"
        "参考频率：%5；输出频率：%6 Hz\n相位误差：%7°；积分 FTW：%8\n连续锁定计数：%9；参考边沿计数：%10\n中心码：%11；内部相位字：%12\n"
        "数值来自最近回复；无有效快照或参考无效时，不作为当前测量。")
        .arg(u(0)).arg(u(1)).arg(flags,8,16,QLatin1Char('0')).arg(result,8,16,QLatin1Char('0'))
        .arg(valid && (result & 2U) && !(result & 0x78U) && reference>0 ? QString::number(reference,'f',3)+" Hz" : "无有效参考")
        .arg(65000000.0*u(3)/4294967296.0,0,'f',3).arg(double(s(5))*360.0/4294967296.0,0,'f',5)
        .arg(s(6)).arg(u(7)).arg(u(8)).arg(u(9)).arg(u(10)));
    QStringList words; for(int i=0;i<16;++i) words << QString::number(u(i));
    m_pllDetails->setToolTip("原始 16 字："+words.join(", "));
    m_pllStatus->setStyleSheet(valid && known && (result & 7U)==7U && !(result & 0x78U) && (flags & 8U) && u(4)
        ? "color:#167343;" : "color:#9a5b12;");
}

bool DacWidget::usesTargetAmplitude() const
{
    return ui->comboBox_amplitudeMode->currentData().toInt() == 1;
}

void DacWidget::refreshAmplitudeMode()
{
    const bool target = usesTargetAmplitude();
    ui->label_amplitude->setVisible(!target);
    ui->spinBox_amplitude->setVisible(!target);
    ui->label_targetVpp->setVisible(target);
    ui->spinBox_targetVpp->setVisible(target);
    ui->spinBox_targetVpp->setEnabled(target);
    ui->amplitudeLoopPanel->setVisible(target);
    if (!target && m_loopRecording) m_loopRecording->setChecked(false);
    ui->label_amplitudeUsage->setText(target
        ? "将 DAC 输出或测量点接入所选 ADC 通道，填写目标峰峰值后点击下发配置。自动稳幅支持正弦波、1 kHz～1 MHz，实际可达幅值取决于输出能力。"
        : "按百分比设置输出幅度，0% 无交流摆幅，100% 为满比例。修改方式或数值后点击下发配置生效。");
}

void DacWidget::selectAmplitudeLoopMode()
{
    setAmplitudeLoopMode(true);
    setSweepControlsVisible(false);
    ui->comboBox_amplitudeMode->setCurrentIndex(1);
    ui->spinBox_targetVpp->setFocus();
}

void DacWidget::setSweepControlsVisible(bool visible)
{
    ui->groupBox_2->setVisible(visible);
}

void DacWidget::setAmplitudeLoopMode(bool enabled)
{
    m_amplitudeLoopMode = enabled;
    if (!enabled) ui->comboBox_amplitudeMode->setCurrentIndex(0);
    refreshAmplitudeMode();
    ui->groupBox->setTitle(enabled ? "固定波形与幅值闭环" : "固定波形");
    ui->label_protocolNotice->setText(enabled
        ? "峰峰值模式使用 ADC 反馈自动稳幅，支持正弦波、1 kHz～1 MHz。填写目标并下发配置；直流偏置为开环设置。"
        : "频率输入 Hz，直流偏置输入 V。选择幅值比例或目标峰峰值，修改后点击下发配置生效。");
}

void DacWidget::setCircuitTestMode(bool enabled)
{
    m_circuitTestMode = enabled;
    m_circuitPanel->setVisible(enabled);
    ui->groupBox->setVisible(enabled ? m_circuitPanel->showManualControls() : !m_digitalPllMode);
    if (enabled) ui->groupBox_2->hide();
    ui->label_dacTitle->setText(enabled ? "待测电路测试" : "DAC 信号发生");
    ui->textEdit_log->setMinimumHeight(enabled ? 70 : 100);
    ui->textEdit_log->setMaximumHeight(enabled ? 90 : 16777215);
    ui->checkBox_receiveHex->setText(enabled ? "HEX 摘要" : "显示接收 HEX 摘要");
    ui->connectionLayout->setAlignment(enabled ? Qt::AlignTop : Qt::Alignment());
    for (QWidget *control : {static_cast<QWidget *>(ui->label_ddsClock),
         static_cast<QWidget *>(ui->spinBox_ddsClock), static_cast<QWidget *>(ui->label_ddsBits),
         static_cast<QWidget *>(ui->label_protocolNotice)}) control->setVisible(!enabled);
    ui->dacTopLayout->setDirection(width() < (enabled ? 700 : 1000) ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
}

void DacWidget::showLoopStatus(const QByteArray &payload)
{
    if (payload.size() != 64) return;
    auto u = [&](int index) { return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(payload.constData() + index*4)); };
    auto s = [&](int index) { return qFromLittleEndian<qint32>(reinterpret_cast<const uchar *>(payload.constData() + index*4)); };
    if (u(0) != 1) {
        const QString message = QString("不支持的闭环状态版本：%1；未解析，不能用旧结果判断当前状态。").arg(u(0));
        ui->label_loopStatus->setStyleSheet("color: #b45309;");
        ui->label_loopStatus->setText(message);
        ui->label_loopStatus->setToolTip(message);
        ui->textEdit_log->appendPlainText(message);
        return;
    }
    const bool processReply=m_loopQueryPending;m_loopQueryPending=false;m_loopReplyTimer->stop();
    m_analysisHistory.loop(payload,!m_autoTaskActive);
    const quint32 stateCode = u(1);
    const quint32 flags = u(2); // Payload flags; not the SCP1 header flags.
    const auto bit = [flags](int index) { return (flags & (quint32(1) << index)) != 0; };
    const auto yesNo = [](bool value) { return value ? QString("是") : QString("否"); };
    const QStringList states = {"稳幅停用", "等待 ADC 标定（兼容状态）", "等待 DAC 标定（兼容状态）",
        "等待直流耦合确认（兼容状态）", "等待采集（兼容状态）", "等待有效测量 / 尚未稳幅",
        "稳幅运行（不代表已达标）", "输出限幅，目标未达到", "ADC 削顶",
        "反馈弱或无有效信号", "频率不支持（兼容状态）"};
    const QString state = stateCode < quint32(states.size()) ? states.at(int(stateCode))
        : QString("未知状态 %1").arg(stateCode);
    // Validity is not implied by state=6. Waiting/disabled/fault states can
    // retain values from older windows, even with cached measurement flags.
    const bool comparable = (stateCode == 6 || stateCode == 7) && bit(0) && bit(1)
        && bit(6) && !bit(7) && u(4) > 0 && u(3) <= 1 && u(15) <= 1;
    const bool fault = (stateCode >= 7 && stateCode <= 10) || bit(7) || bit(10);
    const bool caution = !fault && (bit(2) || (flags & ~quint32(0xFFFu)) != 0
        || (stateCode != 0 && (stateCode != 6 || !comparable)));
    ui->label_loopStatus->setStyleSheet(fault ? "color: #b42318; font-weight: bold;"
        : caution ? "color: #b45309;" : "color: #234369;");
    const QString channel = u(3) == 0 ? "A" : u(3) == 1 ? "B" : QString("未知(%1)").arg(u(3));
    QString summary = QString("%1 · 反馈 %2 通道\n目标 %3 Vpp / 板端基波实测 %4 Vpp")
        .arg(state, channel).arg(u(4)/1000.0, 0, 'f', 3).arg(u(6)/1000.0, 0, 'f', 3);
    if (comparable) {
        const double errorVolts = (double(u(6)) - double(u(4))) / 1000.0;
        const double errorPercent = (double(u(6)) - double(u(4))) / double(u(4)) * 100.0;
        const auto signedNumber = [](double value, int decimals) {
            return (value > 0 ? "+" : "") + QString::number(value, 'f', decimals);
        };
        summary += QString("\nVpp 误差（实测−目标）：%1 Vpp（%2%）；是否达标需按实验容差判断。")
            .arg(signedNumber(errorVolts, 3), signedNumber(errorPercent, 2));
    } else {
        summary += "（最近返回值）\n当前误差未计算：停用、等待或测量有效性未满足；实测值与 gain 不能据此判断收敛。";
    }
    const QString detail = QString("状态码：%1；功能/标定标志：0x%2\n"
        "板端偏置设定（校准后下发值）：%3 V；实测直流偏置：%4 V\nDMA 诊断最小/最大规范化码：%5 / %6；平均码：%7；完整周期数：%8\n"
        "DAC 幅值系数 gain：%9（%10%）%11；数字偏置 bias_codes：%12 码；有符号 DAC 跨度：%13 V；ADC 格式：%14")
        .arg(stateCode).arg(flags, 8, 16, QLatin1Char('0')).arg(s(5)/1000.0, 0, 'f', 3)
        .arg(s(7)/1000.0, 0, 'f', 3).arg(u(8)).arg(u(9)).arg(u(10)/65536.0, 0, 'f', 4)
        .arg(u(11)).arg(u(12)/65536.0, 0, 'f', 6).arg(u(12)/65536.0*100.0, 0, 'f', 2)
        .arg(comparable ? "" : "（最近返回值）").arg(s(13)/65536.0, 0, 'f', 6)
        .arg(s(14)/1000.0, 0, 'f', 3)
        .arg(u(15) == 0 ? "偏移二进制" : u(15) == 1 ? "补码" : QString("未知(%1)").arg(u(15)));
    QStringList decoded;
    const int indices[] = {0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    const QStringList names = {"控制运行", "幅值目标启用", "ADC 参数有效", "直流耦合诊断标记",
        "DAC 跨度有效", "测量有效", "削顶", "ADC 默认参数", "DAC 默认参数", "偏置限幅", "开环偏置已配置"};
    for (int i = 0; i < names.size(); ++i) decoded << names.at(i) + "：" + yesNo(bit(indices[i]));
    if (bit(2)) decoded << "保留 bit2 被置位（协议要求为 0）";
    const quint32 unknownFlags = flags & ~quint32(0xFFFu);
    if (unknownFlags) decoded << QString("未定义标志：0x%1").arg(unknownFlags, 8, 16, QLatin1Char('0'));
    const QString feedback = summary + "\n" + detail + "\n" + decoded.join("；")
        + "\nDMA 诊断窗口与基波估计可能不同步，不能用该组极值直接反推本次基波 Vpp。"
        + "\n直流耦合标记仅用于诊断，不代表模拟耦合已切换。"
        + QString("\n状态载荷版本：%1").arg(u(0));
    ui->label_loopStatus->setText(feedback);
    ui->label_loopStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ui->label_loopStatus->setToolTip(feedback);
    if(!processReply)ui->textEdit_log->appendPlainText("闭环状态：" + feedback);
}

void DacWidget::observeReceivedBytes(const QByteArray &bytes)
{
    if (bytes.isEmpty()) return;
    m_receivedBytes += quint64(bytes.size());
    ++m_receivedChunks;
    m_hexPreview = bytes.left(32);
    m_hasNewData = true;
}

void DacWidget::refreshConnectionState()
{
    const auto state = m_socket ? m_socket->state() : QAbstractSocket::UnconnectedState;
    const bool idle = state == QAbstractSocket::UnconnectedState;
    const bool connected = state == QAbstractSocket::ConnectedState;
    const bool retrying = idle && m_controller && m_controller->connectionRequested();
    ui->lineEdit_ip->setEnabled(idle && !retrying);
    ui->spinBox_port->setEnabled(idle && !retrying);
    ui->btn_connect->setText(retrying ? "停止重连" : idle ? "连接" : connected ? "断开" : "取消连接");
    ui->label_connectionStatus->setText(retrying ? "等待重连…" : idle ? "未连接" : connected ? "TCP 已连接" : "正在连接…");
    ui->groupBox->setEnabled(connected && m_modeStep == ModeStep::Idle);
    ui->groupBox_2->setEnabled(connected && m_modeStep == ModeStep::Idle);

    const bool pllReady = connected && m_modeStep==ModeStep::Idle;
    m_pllSettings->setEnabled(pllReady);
    m_pllStart->setEnabled(pllReady); m_pllStop->setEnabled(pllReady);
    m_pllQuery->setEnabled(pllReady && !(m_pllQueryPending && m_pllManualQuery));
    m_pllClose->setEnabled(pllReady);
    m_pllAutoQuery->setEnabled(connected);
    m_circuitPanel->setConnectionState(connected, m_modeStep != ModeStep::Idle);
}

void DacWidget::updateReceiveStatistics()
{
    ui->label_receiveStatistics->setText(QString("累计接收：%1 字节 / %2 个数据块")
        .arg(m_receivedBytes).arg(m_receivedChunks));
}

void DacWidget::beginOutputChange(const QStringList &commands, const QString &resultText, bool stopPll)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        ui->textEdit_log->appendPlainText("未连接 FPGA，新配置未发送。");
        m_circuitPanel->outputChangeFailed("未连接 FPGA。");
        return;
    }
    if (m_modeStep != ModeStep::Idle) return;
    if (!commands.contains("SWCAL") && !commands.contains("SWSTART")) m_circuitPanel->markStopping();
    m_pllAutoQuery->setChecked(false);
    m_pendingOutput = commands;
    m_pendingResult = resultText;
    m_stopPll = stopPll;
    m_pllStopSent = false;
    // Board modes survive Qt restart/reconnect. Starting PI must establish
    // PLL-off even when this process has never seen the old PLL1 request.
    for (const QString &command : commands) {
        if (command.startsWith("VPP") || command == "LOOP1" || command == "SWCAL" || command == "SWSTART") m_pllMayBeActive = true;
    }
    // Ordinary DAC does not require a PLL status reply before dispatch.
    // Only modes actually requested/reported in this session need an exit.
    // In particular, idle SWSTOP has no ACK and must never be a prerequisite.
    if (m_autoTaskActive) {
        m_modeStep = ModeStep::StopTask;
        m_lastModeReply.clear();
        m_modeStatus->setText("正在自动停止已知板端任务，完成后下发配置…");
        m_modeDeadline->start();
        refreshConnectionState();
        if (!sendCommands({"SWSTOP"})) cancelOutputChange("停止板端任务命令发送失败。");
    } else if (m_manualLoopPossible) {
        beginLoopStop();
    } else {
        beginPllStop();
    }
}

void DacWidget::beginLoopStop()
{
    m_modeStep = ModeStep::StopManual;
    m_lastModeReply.clear();
    m_modeStatus->setText("正在自动停止已知稳幅任务，完成后继续…");
    m_modeDeadline->start();
    refreshConnectionState();
    if (!sendCommands({"LOOP0"})) cancelOutputChange("停止稳幅命令发送失败。");
}

void DacWidget::beginPllStop()
{
    if (!m_stopPll || !m_pllMayBeActive) { finishOutputChange(); return; }
    m_modeStep = ModeStep::StopPll;
    m_lastModeReply.clear();
    m_queryPending = false;
    m_modeStatus->setText("正在自动停用此前启用的 PLL，完成后下发配置…");
    m_modeDeadline->start();
    refreshConnectionState();
    if (!sendCommands({"PLL0"})) { cancelOutputChange("停用 PLL 命令发送失败。"); return; }
    m_pllStopSent = true;
    m_modePollTimer->start();
    pollModeStatus();
}

void DacWidget::pollModeStatus()
{
    // Drain a prior user query before issuing the exit query, so its older
    // reply cannot satisfy a new transition. Queries never gate cold DAC.
    if (m_modeStep != ModeStep::StopPll || m_queryPending || m_pllQueryPending) return;
    m_queryPending = true;
    if (!sendCommands({"PLLSTATUS"})) cancelOutputChange("查询 PLL 停用状态失败。");
}

void DacWidget::modeConfirmationTimedOut()
{
    if (m_modeStep == ModeStep::Idle) return;
    QString reason = m_modeStep == ModeStep::StopPll ? "此前启用的 PLL 未确认退出"
        : m_modeStep == ModeStep::StopManual ? "此前运行的稳幅任务未返回退出完成帧"
        : "此前运行的板端任务未返回退出完成帧";
    if (!m_lastModeReply.isEmpty()) reason += "；最近回复：" + m_lastModeReply;
    cancelOutputChange(reason + "。等待 6 秒，本次新配置未发送；可重试停止或重新下发，不自动重发。");
}

void DacWidget::finishOutputChange()
{
    QStringList commands = m_pendingOutput;
    // Always explicitly release PLL ownership when taking ordinary DDS output.
    // A fresh Qt process cannot infer the board is idle from its own empty state.
    // The PS applies PLL0 after its DDS queue drains; plain DDS needs no status
    // snapshot. Starting VPP/LOOP1 is handled by the confirmed stop above.
    if (m_stopPll && !m_pllStopSent && !commands.isEmpty() && !commands.contains("PLL1"))
        commands.prepend("PLL0");
    const QString result = m_pendingResult;
    m_pendingOutput.clear();
    m_pendingResult.clear();
    m_modePollTimer->stop();
    m_modeDeadline->stop();
    m_queryPending = false;
    m_modeStep = ModeStep::Idle;
    if (commands.isEmpty() || sendCommands(commands)) {
        if (commands.contains("SWCAL") || commands.contains("SWSTART")) {
            m_autoTaskActive = true;
            m_manualLoopPossible = false;
            m_expectedTaskStage = commands.contains("SWCAL") ? 0 : 2;
            m_circuitPanel->outputDispatched(commands);
        }
        const bool startingPll = commands.contains("PLL1");
        for (const QString &command : commands) {
            if ((command.startsWith("VPP") && command != "VPP0") || command == "LOOP1")
                m_manualLoopPossible = true;
        }
        m_modeStatus->setText(result);
        ui->label_loopStatus->setText(result);
        ui->label_loopStatus->setStyleSheet("color:#234369;");
        if (startingPll) {
            m_pllMayBeActive = true;
            if (m_pllQueryPending) m_pllQueryObsolete = true;
            m_pllStatus->setText("PLL 启用请求已发送，尚未确认配置生效或锁定");
            if (m_digitalPllMode) m_pllAutoQuery->setChecked(true);
            queryDigitalPll();
        }
    } else {
        m_modeStatus->setText("新配置未完整发送，请检查连接。");
        m_circuitPanel->outputChangeFailed("新配置未完整发送，请检查连接。");
    }
    refreshConnectionState();
}

void DacWidget::cancelOutputChange(const QString &reason)
{
    m_circuitPanel->outputChangeFailed(reason);
    m_modeStep = ModeStep::Idle;
    m_pllQueryPending = false;
    m_pllReplyTimer->stop();
    m_pllAutoQuery->setChecked(false);
    m_modePollTimer->stop();
    m_modeDeadline->stop();
    m_pendingOutput.clear();
    m_pendingResult.clear();
    m_queryPending = false;
    m_modeStatus->setText(reason);
    ui->textEdit_log->appendPlainText(reason);
    refreshConnectionState();
}

void DacWidget::handleModeStatus(quint16 type, quint32 flags, quint32 sequence,
                                 quint32 count, const QByteArray &payload)
{
    Q_UNUSED(sequence);
    const bool circuitWasActive = m_circuitPanel->hasTask();
    m_circuitPanel->handleFrame(type, count, payload);
    if (circuitWasActive && !m_circuitPanel->hasTask() && m_modeStep == ModeStep::Idle)
        m_modeStatus->setText("电路任务已结束，测量结果见下方。");
    const auto word = [&payload](int index) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(payload.constData()) + 4 * index);
    };
    if (type == 8 && count == 16 && payload.size() == 64) {
        const bool userQueryReply = m_pllQueryPending;
        m_pllQueryPending = false;
        m_pllReplyTimer->stop();
        if (!userQueryReply || !m_pllQueryObsolete) showPllStatus(flags, payload);
        m_pllQueryObsolete = false;
        const bool usable = !(flags & 0xC0000001U) && (flags & 4U);
        // An old disabled observation must not erase a newly sent PLL1.
        // Clear the active intent only after the explicit PLL0 exit query.
        if (usable && (word(2) & 1U)) m_pllMayBeActive = true;
        if (m_modeStep == ModeStep::StopPll && m_queryPending && !userQueryReply) {
            m_queryPending = false;
            m_lastModeReply = QString("PLLSTATUS flags=0x%1，启用=%2")
                .arg(flags, 8, 16, QLatin1Char('0')).arg(word(2) & 1U);
            if (flags & 0x80000000U) cancelOutputChange("PLL 硬件 ID 不匹配，此次切换配置未发送。");
            else if (usable && !(word(2) & 1U)) {
                m_pllMayBeActive = false;
                finishOutputChange();
            }
        }
        refreshConnectionState();
        return;
    }
    if (type == 9 && count == 24 && payload.size() == 96 && word(0) == 1) {
        if (word(1) == 3) m_manualLoopPossible = true;
        else if (word(1) <= 2) {
            m_autoTaskActive = true;
            m_manualLoopPossible = false;
            m_expectedTaskStage = word(1) == 0 ? 0 : 2;
            if (m_modeStep != ModeStep::Idle && m_modeStep != ModeStep::StopTask)
                cancelOutputChange("切换期间收到另一项板端任务数据，本次配置未发送，请重试。");
        }
        return;
    }
    if (type == 11 && count == 16 && payload.size() == 64 && word(0) == 1) {
        m_autoTaskActive = true;
        m_manualLoopPossible = false;
        m_expectedTaskStage = 4;
        if (m_modeStep != ModeStep::Idle && m_modeStep != ModeStep::StopTask)
            cancelOutputChange("切换期间收到板端导出数据，本次配置未发送，请重试。");
        return;
    }
    if (type == 10 && count == 16 && payload.size() == 64 && word(0) == 1) {
        const quint32 stage = word(1);
        if (stage != 0 && stage != 2 && stage != 3 && stage != 4) return;
        if (stage == 3) {
            m_manualLoopPossible = false;
            if (m_modeStep == ModeStep::StopManual) {
                // LOOP0 was already sent and its preserving write has finished.
                if (!m_pendingOutput.isEmpty() && m_pendingOutput.first() == "LOOP0")
                    m_pendingOutput.removeFirst();
                beginPllStop();
            }
        } else if (m_expectedTaskStage == int(stage)) {
            m_autoTaskActive = false;
            m_manualLoopPossible = false;
            m_expectedTaskStage = -1;
            if (m_modeStep == ModeStep::StopTask) {
                beginPllStop();
            }
        }
        // Completion can carry a retained fault; do not call it experiment success.
        if (word(13)) ui->textEdit_log->appendPlainText(QString("板端任务结束：stage=%1，fault=%2。")
            .arg(stage).arg(word(13)));
        return;
    }
    if (type == 4 && count == 16 && payload.size() == 64 && word(0) == 1 && (word(2) & 1U)
        && !m_autoTaskActive)
        m_manualLoopPossible = true;
}

void DacWidget::parseStandaloneFrames(const QByteArray &bytes)
{
    // Shared mode uses ADC's sole stream reader. Standalone DAC still needs
    // complete binary frames, including TCP splits/coalescing, for status display.
    m_statusBuffer += bytes;
    while (true) {
        const int magic = m_statusBuffer.indexOf("SCP1");
        if (magic < 0) { m_statusBuffer = m_statusBuffer.right(3); return; }
        if (magic) m_statusBuffer.remove(0, magic);
        if (m_statusBuffer.size() < 40) return;
        const auto p = reinterpret_cast<const uchar *>(m_statusBuffer.constData());
        const quint32 size = qFromLittleEndian<quint32>(p + 36);
        if (qFromLittleEndian<quint16>(p + 4) != 1 || size > 64U * 1024U * 1024U) {
            m_statusBuffer.remove(0, 1); continue;
        }
        if (quint32(m_statusBuffer.size() - 40) < size) return;
        const quint16 type = qFromLittleEndian<quint16>(p + 6);
        const quint32 flags = qFromLittleEndian<quint32>(p + 12);
        const quint32 sequence = qFromLittleEndian<quint32>(p + 8);
        const quint32 count = qFromLittleEndian<quint32>(p + 32);
        const QByteArray payload = m_statusBuffer.mid(40, int(size));
        m_statusBuffer.remove(0, 40 + int(size));
        if (type == 4 && count == 16 && payload.size() == 64) showLoopStatus(payload);
        handleModeStatus(type, flags, sequence, count, payload);
    }
}

bool DacWidget::sendCommands(const QStringList &commands, bool logRequest)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        ui->textEdit_log->appendPlainText("未连接 FPGA，无法发送 DAC 指令。");
        return false;
    }
    // A single queued buffer preserves command order; TCP itself is a stream.
    const QByteArray bytes = (commands.join("\n") + "\n").toLatin1();
    if (m_socket->write(bytes) != bytes.size()) {
        ui->textEdit_log->appendPlainText("DAC 指令未完整进入发送队列，设备状态未知：" + m_socket->errorString());
        m_socket->abort();
        return false;
    }
    m_analysisHistory.commands(commands);
    if (logRequest) ui->textEdit_log->appendPlainText("已排队发送：" + commands.join(" → ") + "（每条以 \\n 结束；未确认设备执行）");
    return true;
}

bool DacWidget::dcOffsetCommandMillivolts(int &millivolts) const
{
    if(!ui->spinBox_dcOffset->hasAcceptableInput())return false;
    const double coefficient=DacOffsetCalibrationCoefficient;
    const double scaled=ui->spinBox_dcOffset->value()*coefficient*1000.0;
    if(!std::isfinite(coefficient) || coefficient<=0 || !std::isfinite(scaled) || std::abs(scaled)>20000.0)return false;
    millivolts=int(std::round(scaled)); return true; // Keep positive and negative half-mV rounding symmetric.
}

void DacWidget::sendFixedWave()
{
    const bool automaticAmplitude = usesTargetAmplitude();
    if (!ui->spinBox_freq->hasAcceptableInput() || !ui->spinBox_phase->hasAcceptableInput()
        || (!automaticAmplitude && !ui->spinBox_amplitude->hasAcceptableInput()) || !ui->spinBox_dcOffset->hasAcceptableInput()) {
        ui->textEdit_log->appendPlainText("请填写有效的固定波形参数。");
        return;
    }
    int offsetMillivolts=0;
    if(!dcOffsetCommandMillivolts(offsetMillivolts)) {
        ui->textEdit_log->appendPlainText("未下发配置：直流偏置无效或超出允许范围。");
        return;
    }
    if (automaticAmplitude && !ui->spinBox_targetVpp->hasAcceptableInput()) {
        ui->textEdit_log->appendPlainText("请填写有效的目标峰峰值。");
        return;
    }
    if (automaticAmplitude) {
        QString issue;
        if (ui->comboBox_wave->currentData().toInt() != 1)
            issue = "幅值闭环仅支持正弦波 W1，余弦及其他波形不能启用稳幅。";
        else if (ui->spinBox_freq->value() < 1000.0 || ui->spinBox_freq->value() > 1000000.0)
            issue = "幅值闭环频率需在 1 kHz～1 MHz 范围内。";
        else if (ui->comboBox_feedbackChannel->currentIndex() < 0)
            issue = "请选择有效的 ADC 反馈通道。";
        if (!issue.isEmpty()) {
            ui->textEdit_log->appendPlainText("未下发配置：" + issue);
            ui->label_loopStatus->setText("未下发配置：" + issue);
            return;
        }
    }
    const double clockHz = ui->spinBox_ddsClock->value() * 1000000.0;
    const double requestedHz = ui->spinBox_freq->value();
    quint32 ftw = 0;
    if (!ui->spinBox_ddsClock->hasAcceptableInput() ||
        !ddsFrequencyToFtw(requestedHz, clockHz, 32, ftw) || (requestedHz > 0 && ftw == 0)) {
        ui->textEdit_log->appendPlainText("无法换算频率：请确认实际 DDS 时钟；输出频率需在时钟一半以内，非零频率不能小到换算为 0。");
        return;
    }
    const double phaseDegrees = ui->spinBox_phase->value();
    quint32 phaseWord = 0;
    if (!ddsDegreesToPhaseWord(phaseDegrees, phaseWord)) {
        ui->textEdit_log->appendPlainText("相位无效：请输入 0～360°。");
        return;
    }
    ui->textEdit_log->appendPlainText(QString("固定频率：%1 Hz → FTW %2；量化后约 %3 Hz（DDS 时钟 %4 MHz，32 位）。")
        .arg(requestedHz, 0, 'g', 12).arg(ftw)
        .arg(ddsFtwToFrequency(ftw, clockHz, 32), 0, 'g', 12).arg(clockHz / 1000000.0));
    ui->textEdit_log->appendPlainText(QString("相位：%1° → 相位字 %2（P%2；360° 与 0° 等效）。")
        .arg(phaseDegrees, 0, 'f', 3).arg(phaseWord));
    QStringList commands;
    // Disable output before changing the fixed-wave parameters. Keep VPP
    // after E1: subsequent manual DDS commands would exit amplitude control.
    commands << "LOOP0" << "E0" << "S0"
        << "W" + QString::number(ui->comboBox_wave->currentData().toInt())
        << "F" + QString::number(ftw)
        << "P" + QString::number(phaseWord)
        << "A" + QString::number(qRound(ui->spinBox_amplitude->value() * 10.0))
        << "OFFSET" + QString::number(offsetMillivolts) << "E1";
    if (automaticAmplitude) {
        commands << "FBCH" + QString::number(ui->comboBox_feedbackChannel->currentData().toInt())
            << "VPP" + QString::number(quint64(qRound64(ui->spinBox_targetVpp->value() * 1000.0)));
    }
    beginOutputChange(commands, automaticAmplitude ? "目标已发送，闭环执行未确认" : "手动波形配置已发送，执行未确认");
}

void DacWidget::startSweep()
{
    if (usesTargetAmplitude()) {
        ui->textEdit_log->appendPlainText("原始 DDS 扫频使用幅值比例，请先选择“幅值比例（%）”并设置百分比，再开始扫频。");
        return;
    }
    if (!ui->spinBox_startFreq_2->hasAcceptableInput() || !ui->spinBox_stopFreq->hasAcceptableInput()
        || !ui->doubleSpinBox_5->hasAcceptableInput() || !ui->spinBox_amplitude->hasAcceptableInput()) {
        ui->textEdit_log->appendPlainText("请填写有效的扫频参数和幅值比例。");
        return;
    }
    const double startHz = ui->spinBox_startFreq_2->value();
    const double stopHz = ui->spinBox_stopFreq->value();
    const double stepHz = ui->doubleSpinBox_5->value();
    const double clockHz = ui->spinBox_ddsClock->value() * 1000000.0;
    quint32 start = 0, stop = 0, step = 0;
    if (startHz >= stopHz || stepHz <= 0 || stepHz > stopHz - startHz) {
        ui->textEdit_log->appendPlainText("扫频参数无效：起始频率必须小于结束频率，步进需大于 0 且不超过扫频范围。");
        return;
    }
    if (!ui->spinBox_ddsClock->hasAcceptableInput() ||
        !ddsFrequencyToFtw(startHz, clockHz, 32, start) ||
        !ddsFrequencyToFtw(stopHz, clockHz, 32, stop) ||
        !ddsFrequencyToFtw(stepHz, clockHz, 32, step) ||
        (startHz > 0 && start == 0) || start >= stop || step == 0 || step > stop - start) {
        ui->textEdit_log->appendPlainText("扫频换算失败：请确认 DDS 时钟与频率范围；频率或步进过小会导致 FTW 为 0，或起止值量化后重合。");
        return;
    }
    ui->textEdit_log->appendPlainText(QString("扫频 Hz → FTW：起始 %1 → %2，结束 %3 → %4，步进 %5 → %6（时钟 %7 MHz，32 位）。")
        .arg(startHz, 0, 'g', 12).arg(start).arg(stopHz, 0, 'g', 12).arg(stop)
        .arg(stepHz, 0, 'g', 12).arg(step).arg(clockHz / 1000000.0));
    QStringList commands;
    // PL gives fixed-output E priority over sweep S. E1 after S1 would
    // disable sweep; first stop both modes, then activate only S.
    commands << "LOOP0" << "E0" << "S0" << "W" + QString::number(ui->comboBox_wave->currentData().toInt())
        << "A" + QString::number(qRound(ui->spinBox_amplitude->value() * 10.0))
        << "B" + QString::number(start) << "C" + QString::number(stop)
        << "D" + QString::number(step) << "S1";
    QString result = "DDS 扫频配置已发送，执行未确认";
    if (qRound(ui->spinBox_amplitude->value() * 10.0) == 0) {
        const QString warning = "幅值比例为 0%，扫频没有交流摆幅；如需观察扫频波形，请设置非零幅值后重新开始扫频。";
        ui->textEdit_log->appendPlainText(warning);
        result += "；" + warning;
    }
    beginOutputChange(commands, result);
}

DacWidget::~DacWidget()
{
    // Disconnect callbacks before destroying the generated UI.
    if (m_socket) disconnect(m_socket, nullptr, this, nullptr);
    m_connectTimer->stop();
    m_receiveTimer->stop();
    m_modePollTimer->stop();
    m_modeDeadline->stop();
    m_pllRefreshTimer->stop();
    m_pllReplyTimer->stop();
    m_loopRefreshTimer->stop();m_loopReplyTimer->stop();
    if (m_socket && !m_controller) m_socket->abort();
    delete ui;
}

void DacWidget::toggleConnection()
{
    if (m_controller) {
        m_controller->toggleSharedConnection(ui->lineEdit_ip->text(), quint16(ui->spinBox_port->value()));
        return;
    }
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_connectTimer->stop();
        m_socket->abort();
        return;
    }
    QHostAddress address;
    if (!address.setAddress(ui->lineEdit_ip->text().trimmed()) ||
        address.protocol() != QAbstractSocket::IPv4Protocol || address.isNull() ||
        address == QHostAddress(QHostAddress::AnyIPv4) || address == QHostAddress(QHostAddress::Broadcast)) {
        ui->textEdit_log->appendPlainText("请输入有效的 FPGA IPv4 地址。");
        return;
    }
    const quint16 port = quint16(ui->spinBox_port->value());
    ui->textEdit_log->appendPlainText(QString("正在连接 TCP %1:%2…").arg(address.toString()).arg(port));
    m_socket->connectToHost(address, port);
    if (m_socket->state() != QAbstractSocket::UnconnectedState &&
        m_socket->state() != QAbstractSocket::ConnectedState) m_connectTimer->start();
}

void DacWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    ui->dacTopLayout->setDirection(width() < (m_circuitTestMode ? 700 : 1000) ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    ui->outputLayout->setDirection(width() < 1000 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_pllColumns->setDirection(width() < 1000 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
}
