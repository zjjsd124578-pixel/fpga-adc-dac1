#include "adcwidget.h"
#include "ui_adcwidget.h"
#include "waveformwidget.h"//真正要使用，不是前向声明
#include "app_style.h"//全局样式表
#include <QVBoxLayout>
#include <QtMath>
#include <limits>
#include <QPushButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>

namespace {
const int HeaderBytes = 40;
const quint32 MaxPayloadBytes = 64u * 1024u * 1024u;
const int DisplayPoints = 3000;
// AN9238: -5V~+5V, 12-bit offset binary, 0V 对应 2048
const double AdcVrefMin = -5.0;
const double AdcVrefMax =  5.0;
const double AdcMidCode =  2048.0;
const double AdcFullScale = 4096.0;
bool validPayloadLayout(quint16 type, quint32 count, quint32 bytes)
{
    switch (type) {
    case 1: // ADC preview: one uint32 contains both channels.
    case 2: // Frozen record.
        return quint64(count) * 4u == quint64(bytes);
    case 3: // count is the number of valid samples, not the payload length.
        return bytes == 0;
    case 4: // Retain compatibility with the older 12-byte status variant.
        return (count == 16 && bytes == 64) || (count == 0 && bytes == 12);
    case 8: // PLL status.
    case 10: // Sweep summary/completion.
    case 11: // Calibration/reference metadata.
        return count == 16 && bytes == 64;
    case 9: // Sweep point.
        return count == 24 && bytes == 96;
    case 12: // Exported reference point.
        return count == 3 && bytes == 12;
    default:
        // Unknown types still have a trustworthy boundary if their v1 header
        // and bounded payload_bytes are valid. Do not search inside the payload.
        return true;
    }
}

QString formatRate(double hz)
{
    if (hz == 0) return QStringLiteral("设备未上报");
    if (hz >= 1000000)
        return QString::number(hz / 1000000.0, 'f', 3) + QStringLiteral(" MSa/s");
    if (hz >= 1000)
        return QString::number(hz / 1000.0, 'f', 3) + QStringLiteral(" kSa/s");
    return QString::number(hz) + QStringLiteral(" Sa/s");
}
}


void AdcWidget::setExperimentAnalysisSource(const std::function<ExperimentAnalysis::Snapshot()> &source)
{
    m_waveform->setExperimentAnalysisSource(source);
}

AdcWidget::AdcWidget(QWidget *parent)
    : QWidget(parent), ui(new Ui::AdcWidget),
      m_isConnected(false)
{
    ui->setupUi(this);
    m_inputStatus = new QLabel(this);
    m_inputStatus->setObjectName("label_inputSignalStatus");
    m_inputStatus->setWordWrap(true);
    ui->verticalLayout_4->addWidget(m_inputStatus);
    auto moreMenu = new QMenu(ui->btn_moreOperations);
    moreMenu->addAction(ui->action_windowSettings);
    moreMenu->addAction(ui->action_dump);
    ui->btn_moreOperations->setMenu(moreMenu);
    ui->btn_connect->setProperty("primary", true);
    ui->btn_arm->setProperty("primary", true);
    setStyleSheet(signalAnalyzerStyle());
    ui->groupBox_3->setTitle("设备连接（ADC / DAC 共用 TCP）");
    ui->btn_connect->setToolTip("两页共用连接；断开会同时停止 ADC / DAC 通信，返回或切换页面不会断开。");
    ui->textEdit_rawData->setMaximumBlockCount(500);
    ui->spinBox_port->setMinimum(1);
    ui->comboBox_mode->setCurrentIndex(0);
    ui->comboBox_mode->setEnabled(false);
    m_gapLogTimer = new QTimer(this);
    m_gapLogTimer->setInterval(5000);
    connect(m_gapLogTimer, &QTimer::timeout, this, &AdcWidget::flushPreviewGapLog);
    resetSamplingDisplay();

    m_waveform = new WaveformWidget(this);
    const auto updatePauseButton = [this]() {
        ui->btn_pauseDisplay->setEnabled(m_waveform->canPauseDisplay());
        ui->btn_pauseDisplay->setText(m_waveform->isDisplayPaused() ? "继续波形" : "暂停波形");
        ui->btn_pauseDisplay->setToolTip(m_waveform->isShowingRecord()
            ? "当前正在查看采集记录，波形已停止更新；点击开始/重新布防返回实时。"
            : "暂停后可放大、移动和测量当前波形；板子继续采集，继续后显示最新波形。");
        if (!m_waveform->isHoldingDisplay() && m_waveform->canPauseDisplay())
            updatePreviewSamplingLabels();
    };
    connect(m_waveform, &WaveformWidget::displayStateChanged, this, updatePauseButton);
    connect(ui->btn_pauseDisplay, &QPushButton::clicked, this, [this]() {
        if (!m_waveform->isHoldingDisplay()) updatePreviewSamplingLabels();
        m_waveform->setDisplayPaused(!m_waveform->isDisplayPaused());
    });
    updatePauseButton();
    connect(m_waveform, &WaveformWidget::requestClosedLoopControl, this, &AdcWidget::requestClosedLoopControl);
    QVBoxLayout *layout = new QVBoxLayout(ui->widget_waveform_container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_waveform);

    m_tcpSocket = new QTcpSocket(this);
    m_udpSocket = new QUdpSocket(this);
    QTimer *connectTimer = new QTimer(this);
    connectTimer->setSingleShot(true);
    connectTimer->setInterval(10000);
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    m_reconnectTimer->setInterval(3000);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (!m_wantConnection || m_tcpSocket->state() != QAbstractSocket::UnconnectedState)
            return;
        ui->textEdit_rawData->appendPlainText(
            QString("正在重连 TCP %1:%2 ...")
                .arg(m_targetAddress.toString()).arg(m_targetPort));
        m_tcpSocket->connectToHost(m_targetAddress, m_targetPort);
    });

    connect(m_tcpSocket, &QTcpSocket::readyRead, this, &AdcWidget::readTcpData);
    connect(m_tcpSocket, &QTcpSocket::stateChanged, this,
            [this, connectTimer](QAbstractSocket::SocketState state) {
        m_isConnected = (state == QAbstractSocket::ConnectedState);
        const bool idle = (state == QAbstractSocket::UnconnectedState);
        ui->lineEdit_ip->setEnabled(idle && !m_wantConnection);
        ui->spinBox_port->setEnabled(idle && !m_wantConnection);
        ui->btn_connect->setText(idle ? (m_wantConnection ? "停止重连" : "连接")
                                      : (m_isConnected ? "断开" : "取消连接"));
        if (state == QAbstractSocket::ConnectingState ||
            state == QAbstractSocket::HostLookupState) {
            if (!connectTimer->isActive()) connectTimer->start();
        } else {
            connectTimer->stop();
        }
        if (idle && m_wantConnection && !m_reconnectTimer->isActive()) {
            ui->textEdit_rawData->appendPlainText("连接中断，3 秒后自动重连。");
            m_reconnectTimer->start();
        }
        emit sharedConnectionChanged();
    });
    connect(connectTimer, &QTimer::timeout, this, [this]() {
        if (m_tcpSocket->state() == QAbstractSocket::ConnectingState ||
            m_tcpSocket->state() == QAbstractSocket::HostLookupState) {
            ui->textEdit_rawData->appendPlainText("TCP 连接超时（10 秒）：请检查网络及 FPGA 的 TCP 监听。");
            m_tcpSocket->abort();
        }
    });
    connect(m_tcpSocket, &QTcpSocket::connected, this, [this]() {
        m_reconnectTimer->stop();
        m_tcpSocket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        m_tcpBuffer.clear();
        m_discardRecordBytes = 0;
        m_dataBuffer.clear();
        m_dataBufferB.clear();
        resetSamplingDisplay();
        m_waveform->setAutoScroll(!m_recordDisplayActive);
        ui->textEdit_rawData->appendPlainText(
            QString("共享 TCP 已连接 FPGA %1:%2，可同时控制 DAC 和接收 ADC；请点击开始采集。重连后需重新下发控制命令。")
                .arg(m_tcpSocket->peerAddress().toString()).arg(m_tcpSocket->peerPort()));
    });
    connect(m_tcpSocket, &QTcpSocket::disconnected, this, [this]() {
        const bool interruptedRecord = m_tcpBuffer.size() >= 8 && m_tcpBuffer.startsWith("SCP1")
            && readU16LE(m_tcpBuffer, 6) == 2;
        m_tcpBuffer.clear();
        m_discardRecordBytes = 0;
        resetSamplingDisplay();
        if (interruptedRecord)
            ui->label_recordStatus->setText("记录接收中断，未显示不完整数据；原图保留。重连后可重读冻结记录。");
        ui->textEdit_rawData->appendPlainText("TCP 连接已断开。");
    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_tcpSocket, &QAbstractSocket::errorOccurred, this,
#else
    connect(m_tcpSocket,
            static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(&QAbstractSocket::error), this,
#endif
            [this](QAbstractSocket::SocketError error) {
        ui->textEdit_rawData->appendPlainText(
            QString("TCP 错误（%1）：%2").arg(int(error)).arg(m_tcpSocket->errorString()));
        m_tcpSocket->abort();
        m_tcpBuffer.clear();
        m_discardRecordBytes = 0;
        resetSamplingDisplay();
    });

    connect(ui->btn_back, &QPushButton::clicked, this, &AdcWidget::requestBack);
    // on_btn_xxx_clicked 由 setupUi 自动绑定，不能再次手动 connect。
}


AdcWidget::~AdcWidget()
{
    m_wantConnection = false;
    m_reconnectTimer->stop();
    QObject::disconnect(m_tcpSocket, nullptr, this, nullptr);
    m_tcpSocket->abort();
    m_udpSocket->close();
    delete ui;
}


QString AdcWidget::connectionIp() const { return ui->lineEdit_ip->text(); }
quint16 AdcWidget::connectionPort() const { return quint16(ui->spinBox_port->value()); }

void AdcWidget::toggleSharedConnection(const QString &ip, quint16 port)
{
    if (!m_wantConnection) {
        ui->lineEdit_ip->setText(ip.trimmed());
        ui->spinBox_port->setValue(port);
    }
    on_btn_connect_clicked();
}

void AdcWidget::on_btn_connect_clicked()
{
    if (m_wantConnection) {
        m_wantConnection = false;
        m_reconnectTimer->stop();
        m_tcpSocket->abort();
        m_tcpBuffer.clear();
        m_discardRecordBytes = 0;
        ui->lineEdit_ip->setEnabled(true);
        ui->spinBox_port->setEnabled(true);
        ui->btn_connect->setText("连接");
        ui->textEdit_rawData->appendPlainText("已断开连接并停止自动重连。");
        emit sharedConnectionChanged();
        return;
    }
    const QString ip = ui->lineEdit_ip->text().trimmed();
    QHostAddress address;
    if (!address.setAddress(ip) || address.protocol() != QAbstractSocket::IPv4Protocol ||
        address == QHostAddress(QHostAddress::AnyIPv4)) {
        ui->textEdit_rawData->appendPlainText("请输入有效的 FPGA IPv4 地址，例如 192.168.1.10。");
        return;
    }
    m_targetAddress = address;
    m_targetPort = quint16(ui->spinBox_port->value());
    m_wantConnection = true;
    m_reconnectTimer->stop();
    m_tcpBuffer.clear();
    m_discardRecordBytes = 0;
    m_dataBuffer.clear();
    m_dataBufferB.clear();
    resetSamplingDisplay();
    m_waveform->setAutoScroll(!m_recordDisplayActive);
    if (!m_recordDisplayActive) m_waveform->setWaveformData(m_dataBuffer, m_dataBufferB, DisplayPoints);
    ui->lineEdit_ip->setEnabled(false);
    ui->spinBox_port->setEnabled(false);
    emit sharedConnectionChanged();
    ui->textEdit_rawData->appendPlainText(QString("正在连接 TCP %1:%2 ...").arg(ip).arg(m_targetPort));
    m_tcpSocket->connectToHost(m_targetAddress, m_targetPort);
}

bool AdcWidget::sendCommand(const QString &cmd)
{
    if (m_tcpSocket->state() != QAbstractSocket::ConnectedState) {
        ui->textEdit_rawData->appendPlainText("发送失败：TCP 尚未连接。");
        return false;
    }
    const QByteArray data = (cmd + "\n").toUtf8();
    const qint64 written = m_tcpSocket->write(data);
    if (written != data.size()) {
        ui->textEdit_rawData->appendPlainText(
            QString("命令写入失败（%1/%2）：%3")
                .arg(written).arg(data.size()).arg(m_tcpSocket->errorString()));
        m_tcpSocket->abort();
        return false;
    }
    ui->textEdit_rawData->appendPlainText("命令已加入发送缓冲区：" + cmd);
    QString pending;
    if (cmd == "ARM" || cmd.startsWith("WINDOW ")) {
        resetSamplingDisplay(true);
        m_waveform->setAutoScroll(true);
        pending = cmd == "ARM" ? "已请求开始/重新布防，等待板端状态；记录上传忙时可能不会立即生效。"
            : "已请求设置采集窗口；生效后重新布防，尚未确认板端执行。";
    } else if (cmd == "TRIGGER") {
        pending = "已请求等待硬件阈值触发，不代表已经触发。";
    } else if (cmd == "PAUSE") {
        pending = "已请求结束采集；仍需采完触发后数据，再冻结并上传记录。";
        ui->label_recordStatus->setText("记录：等待板端完成采集并上传。");
    } else if (cmd == "DUMP") {
        ui->label_recordStatus->setText("记录：已请求重读；未冻结、为空或上传忙时不执行。");
    }
    if (!pending.isEmpty()) {
        ui->label_acquisitionStatus->setText(pending);
        ui->label_acquisitionStatus->setStyleSheet("color: #b45309;");
        ui->label_acquisitionStatus->setToolTip(pending + "状态帧是快照，不是命令应答。");
    }
    return true;
}

void AdcWidget::on_btn_arm_clicked()     { sendCommand("ARM"); }
void AdcWidget::on_btn_trigger_clicked() { sendCommand("TRIGGER"); }
void AdcWidget::on_action_dump_triggered() { sendCommand("DUMP"); }
void AdcWidget::on_btn_pause_clicked()   { sendCommand("PAUSE"); }

void AdcWidget::on_action_windowSettings_triggered()
{
    if (m_windowSettingsDialog) {
        m_windowSettingsDialog->show();
        m_windowSettingsDialog->raise();
        m_windowSettingsDialog->activateWindow();
        return;
    }
    QDialog *dialog = new QDialog(this);
    m_windowSettingsDialog = dialog;
    dialog->setObjectName("acquisitionWindowDialog");
    dialog->setWindowTitle("采集窗口设置");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMinimumWidth(520);
    dialog->setStyleSheet(signalAnalyzerStyle() + "QDialog { background: #f3f6fa; }");
    QVBoxLayout *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(14);
    QLabel *hint = new QLabel("设置触发点前后的记录时间，中心窗口也影响预览抽点间隔。应用后重新布防；数值为待发送设置。当前完整记录显示上限为 64 MiB，65 MSa/s 时总缓存约 258 ms。", dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    QFormLayout *form = new QFormLayout();
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto input = [dialog](const char *name, double minimum, quint32 microseconds) {
        QDoubleSpinBox *spin = new QDoubleSpinBox(dialog);
        spin->setObjectName(name);
        spin->setDecimals(3);
        spin->setRange(minimum, 3400.0);
        spin->setSingleStep(1.0);
        spin->setSuffix(" ms");
        spin->setValue(microseconds / 1000.0);
        return spin;
    };
    QDoubleSpinBox *view = input("spinBox_windowViewMs", 1.0, m_windowViewUs);
    QDoubleSpinBox *before = input("spinBox_windowBeforeMs", 0.0, m_windowBeforeUs);
    QDoubleSpinBox *after = input("spinBox_windowAfterMs", 0.0, m_windowAfterUs);
    form->addRow("中心窗口", view);
    form->addRow("额外触发前缓存", before);
    form->addRow("额外触发后缓存", after);
    layout->addLayout(form);
    QLabel *preview = new QLabel(dialog);
    preview->setObjectName("label_windowPreview");
    preview->setWordWrap(true);
    layout->addWidget(preview);
    QLabel *status = new QLabel(dialog);
    status->setObjectName("label_windowRequestStatus");
    status->setWordWrap(true);
    layout->addWidget(status);
    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->button(QDialogButtonBox::Close)->setText("关闭");
    QPushButton *apply = buttons->addButton("应用并重新布防", QDialogButtonBox::ActionRole);
    apply->setObjectName("btn_applyAcquisitionWindow");
    apply->setProperty("primary", true);
    layout->addWidget(buttons);
    const auto us = [](QDoubleSpinBox *spin) { return quint64(qRound64(spin->value() * 1000.0)); };
    const auto valid = [=]() {
        return view->hasAcceptableInput() && before->hasAcceptableInput() && after->hasAcceptableInput()
            && us(view) >= 1000 && us(view) + us(before) + us(after) <= 3400000;
    };
    const auto refresh = [=]() {
        if (!valid()) {
            preview->setText("中心窗口至少 1 ms，三项合计不能超过 3400 ms；请检查输入。");
            status->setText("参数无效，未发送。");
            status->setStyleSheet("color: #b42318;");
            return;
        }
        const quint64 v = us(view), b = us(before), a = us(after);
        const quint64 pre = v / 2 + b, post = v - v / 2 + a;
        preview->setText(QString("总缓存：%1 ms\n触发前：%2 ms；触发后：%3 ms\n待发送：WINDOW %4 %5 %6")
            .arg((v+b+a)/1000.0, 0, 'f', 3).arg(pre/1000.0, 0, 'f', 3).arg(post/1000.0, 0, 'f', 3)
            .arg(v).arg(b).arg(a));
        status->setText("尚未下发；点击应用后以整数 μs 发送。最小窗口 1 ms，总缓存上限 3400 ms。");
        status->setStyleSheet("color: #526780;");
    };
    for (QDoubleSpinBox *spin : {view, before, after})
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), dialog, [=](double) { refresh(); });
    connect(apply, &QPushButton::clicked, dialog, [=]() {
        if (!valid()) { refresh(); return; }
        const quint32 v = quint32(us(view)), b = quint32(us(before)), a = quint32(us(after));
        if (!sendCommand(QString("WINDOW %1 %2 %3").arg(v).arg(b).arg(a))) {
            status->setText("发送失败，请检查 TCP 连接；不能认定板端窗口已改变。");
            status->setStyleSheet("color: #b42318;");
            return;
        }
        m_windowViewUs = v; m_windowBeforeUs = b; m_windowAfterUs = a;
        status->setText("请求已加入发送队列；窗口生效后会重新布防，尚未确认板端执行。");
        status->setStyleSheet("color: #b45309;");
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    refresh();
    dialog->show();
}

void AdcWidget::resetSamplingDisplay(bool clearRecord)
{
    if (clearRecord && m_waveform && m_waveform->isDisplayPaused())
        m_waveform->setDisplayPaused(false);
    if (clearRecord && m_recordDisplayActive) {
        m_recordDisplayActive = false;
        m_waveform->setWaveformData(QVector<double>());
        m_waveform->resetZoom();
        m_dataBuffer.clear(); m_dataBufferB.clear();
    }
    if (!m_recordDisplayActive) {
        m_inputWarnings.clear();
        m_inputDiagnostic = "尚未收到本轮预览数据，不能判断输入信号。";
        if (m_inputStatus) {
            m_inputStatus->setText("输入信号：待预览数据；图中电压是采样换算值，不是默认波形。");
            m_inputStatus->setToolTip(m_inputDiagnostic);
            m_inputStatus->setStyleSheet("color:#526780;");
        }
    }
    m_acquisitionStatusKnown = false;
    m_acquisitionFlags = 0;
    m_acquisitionOverflowSeen = false;
    ui->label_acquisitionStatus->setText("采集状态：待板端上报。");
    ui->label_acquisitionStatus->setStyleSheet("color: #526780;");
    ui->label_acquisitionStatus->setToolTip(QString());
    if (!m_recordDisplayActive) ui->label_recordStatus->setText("记录：尚未收到完整记录。");
    m_reportedProtocolTypes.clear();
    flushPreviewGapLog();
    m_gapLogTimer->stop();
    m_gapCount = 0;
    m_pendingGapCount = 0;
    m_previewQualityWarnings.clear();
    m_previewContinuous = false;
    m_previewRate = 0;
    m_previewStride = 0;
    m_reportedPreviewRate = 0;
    m_reportedPreviewStride = 0;
    if (!m_recordDisplayActive && (!m_waveform || !m_waveform->isHoldingDisplay())) {
        ui->label_sampleRateValue->setText("未收到数据");
        ui->label_channelCountValue->setText("2 路");
        ui->label_adcBitsValue->setText("12 bit");
        ui->label_decimationValue->setText("未收到数据");
    }
    updateAcquisitionWarnings();
}

void AdcWidget::recordPreviewGap(quint64 actualStart)
{
    ++m_gapCount;
    m_lastGapExpected = m_nextPreviewSample;
    m_lastGapActual = actualStart;
    if (m_gapCount == 1) {
        ui->textEdit_rawData->appendPlainText(
            QString("数据不连续：已清空旧历史（预期采样起点 %1，实际 %2）。重复提示每 5 秒汇总；不代表已确认 DMA 溢出。")
                .arg(m_lastGapExpected).arg(m_lastGapActual));
        m_gapLogTimer->start();
    } else {
        ++m_pendingGapCount;
    }
}

void AdcWidget::flushPreviewGapLog()
{
    if (m_pendingGapCount == 0) return;
    ui->textEdit_rawData->appendPlainText(
        QString("采样连续性汇总：新增 %1 次不衔接，本轮累计 %2 次；最近预期起点 %3，实际 %4。DMA 状态请查看采集控制区。")
            .arg(m_pendingGapCount).arg(m_gapCount).arg(m_lastGapExpected).arg(m_lastGapActual));
    m_pendingGapCount = 0;
}

void AdcWidget::updateAcquisitionWarnings()
{
    if (!m_waveform || m_recordDisplayActive) return;
    QStringList warnings = m_previewQualityWarnings;
    warnings.append(m_inputWarnings);
    if (!m_acquisitionStatusKnown)
        warnings << "DMA 状态未确认：尚未收到本轮 type 3 采集状态，不能按预览帧 flags 判定溢出。";
    else if (m_acquisitionOverflowSeen)
        warnings << "DMA 溢出：本轮 type 3 采集状态曾报告 bit3=1；不能仅凭后续波形外观判定数据完整。";
    else
        warnings << "DMA 状态：最近 type 3 未报告溢出；不等于已证明历史数据连续。";
    if (m_acquisitionStatusKnown && (m_acquisitionFlags & ~quint32(0xFu)))
        warnings << "采集状态含未定义标志位，已知位以外的含义尚未确认。";
    m_waveform->setAcquisitionWarnings(warnings);
}

void AdcWidget::updateAcquisitionStatus(quint32 flags, quint32 count, quint32 sequence)
{
    const bool changed = !m_acquisitionStatusKnown || flags != m_acquisitionFlags;
    m_acquisitionStatusKnown = true;
    m_acquisitionFlags = flags;
    m_acquisitionOverflowSeen = m_acquisitionOverflowSeen || (flags & 8u);
    QString text = QString("最近板端状态：%1，%2，%3\n有效样本：%4；溢出标志：%5")
        .arg(flags & 1u ? "采集中" : "未采集", flags & 2u ? "已触发" : "未触发",
             flags & 4u ? "记录完成" : "未完成").arg(count).arg(flags & 8u ? "是" : "否");
    if (m_acquisitionOverflowSeen && !(flags & 8u)) text += "（本轮曾报告溢出）";
    if (flags & ~quint32(0xFu)) text += "；含未定义标志";
    ui->label_acquisitionStatus->setText(text);
    ui->label_acquisitionStatus->setStyleSheet(m_acquisitionOverflowSeen ? "color: #b42318;" : "color: #234369;");
    ui->label_acquisitionStatus->setToolTip(QString("type 3；seq=%1；flags=0x%2。此帧是板端状态快照，不是命令应答；点击 ARM 后上位机重新累计本轮状态。")
        .arg(sequence).arg(flags, 8, 16, QLatin1Char('0')));
    if (changed) ui->textEdit_rawData->appendPlainText(text);
    updateAcquisitionWarnings();
}

quint16 AdcWidget::readU16LE(const QByteArray &buf, int offset)
{
    return quint16(quint8(buf[offset])) | (quint16(quint8(buf[offset + 1])) << 8);
}

quint32 AdcWidget::readU32LE(const QByteArray &buf, int offset)
{
    return quint32(quint8(buf[offset]))
         | (quint32(quint8(buf[offset + 1])) << 8)
         | (quint32(quint8(buf[offset + 2])) << 16)
         | (quint32(quint8(buf[offset + 3])) << 24);
}

quint64 AdcWidget::readU64LE(const QByteArray &buf, int offset)
{
    return quint64(readU32LE(buf, offset)) | (quint64(readU32LE(buf, offset + 4)) << 32);
}

void AdcWidget::readTcpData()
{
    const QByteArray bytes = m_tcpSocket->readAll();
    emit tcpBytesReceived(bytes);
    m_tcpBuffer.append(bytes);
    bool updateWaveform = false;
    while (m_discardRecordBytes || m_tcpBuffer.size() >= HeaderBytes) {
        if (m_discardRecordBytes) {
            const int drop = int(std::min(m_discardRecordBytes, quint64(m_tcpBuffer.size())));
            m_tcpBuffer.remove(0, drop); m_discardRecordBytes -= quint64(drop);
            if (m_discardRecordBytes || m_tcpBuffer.isEmpty()) break;
            continue;
        }
        if (!m_tcpBuffer.startsWith("SCP1")) {
            const int next = m_tcpBuffer.indexOf("SCP1", 1);
            const int discarded = next >= 0 ? next : m_tcpBuffer.size() - 3;
            m_tcpBuffer.remove(0, discarded);
            ui->textEdit_rawData->appendPlainText(
                QString("丢弃 %1 字节非 SCP1 数据，请核对 FPGA 输出协议。").arg(discarded));
            continue;
        }
        const quint16 version = readU16LE(m_tcpBuffer, 4);
        const quint16 type = readU16LE(m_tcpBuffer, 6);
        const quint32 seq = readU32LE(m_tcpBuffer, 8);
        const quint32 flags = readU32LE(m_tcpBuffer, 12);
        const quint32 rate = readU32LE(m_tcpBuffer, 16);
        const quint64 startSample = readU64LE(m_tcpBuffer, 20);
        const quint32 stride = readU32LE(m_tcpBuffer, 28);
        const quint32 points = readU32LE(m_tcpBuffer, 32);
        const quint32 bytes = readU32LE(m_tcpBuffer, 36);
        // Keep the existing bounded record size. Drain an oversized payload by
        // its declared length instead of searching for frame magic inside it.
        if (version == 1 && type == 2 && bytes > MaxPayloadBytes && validPayloadLayout(type, points, bytes)) {
            m_discardRecordBytes = bytes;
            m_tcpBuffer.remove(0, HeaderBytes);
            ui->label_recordStatus->setText("记录超过当前 64 MiB 显示上限；请缩短采集窗口或额外缓存后重试。原图保留。");
            ui->textEdit_rawData->appendPlainText(QString("完整记录 %1 点、%2 字节超过上位机显示上限，按帧长度丢弃，不修改现有波形。").arg(points).arg(bytes));
            continue;
        }
        if (version != 1 || bytes > MaxPayloadBytes || !validPayloadLayout(type, points, bytes)) {
            ui->textEdit_rawData->appendPlainText(
                QString("非法帧头：version=%1 type=%2 points=%3 bytes=%4；重新查找帧头。")
                    .arg(version).arg(type).arg(points).arg(bytes));
            m_tcpBuffer.remove(0, 4);
            continue;
        }
        const int frameLength = HeaderBytes + int(bytes);
        if (m_tcpBuffer.size() < frameLength) {
            if (type == 2)
                ui->label_recordStatus->setText(QString("记录：正在接收 %1 / %2 字节，完整接收后显示冻结波形…")
                    .arg(m_tcpBuffer.size() - HeaderBytes).arg(bytes));
            break;
        }

        if (type == 1 && !m_recordDisplayActive) {
            const double effectiveRate = stride > 0 ? double(rate) / stride : 0.0;
            if (m_reportedPreviewRate != rate || m_reportedPreviewStride != stride) {
                ui->textEdit_rawData->appendPlainText(
                    QString("预览时间尺度：原始采样率=%1 Hz，采样间隔=%2，有效采样率=%3 Hz")
                        .arg(rate).arg(stride).arg(effectiveRate, 0, 'g', 10));
                if (stride == 0)
                    ui->textEdit_rawData->appendPlainText("预览采样间隔未上报（0）：无法可靠换算时间和频率，请检查 FPGA 帧头。");
            }
            m_reportedPreviewRate = rate;
            m_reportedPreviewStride = stride;
            m_latestPreviewRate = rate;
            m_latestPreviewStride = stride;
            if (!m_waveform->isHoldingDisplay()) updatePreviewSamplingLabels();
        }

        if (type == 1 && !m_recordDisplayActive) {
            const double effectiveRate = stride > 0 ? double(rate) / stride : 0.0;
            m_waveform->setSampleRateHz(effectiveRate);
            const quint32 first = points > quint32(DisplayPoints) ? points - DisplayPoints : 0;
            // 跳帧、抽取变化、独立预览或截去大帧前段时不能把缓存当连续采样。
            const bool gap = m_previewContinuous && rate == m_previewRate &&
                stride == m_previewStride && startSample != m_nextPreviewSample;
            const bool parametersChanged = m_previewContinuous &&
                (rate != m_previewRate || stride != m_previewStride);
            if (!m_previewContinuous || gap || parametersChanged || first > 0) {
                m_dataBuffer.clear();
                m_dataBufferB.clear();
                m_previewQualityWarnings.clear();
                if (gap) {
                    m_previewQualityWarnings << "数据不连续：采样起始序号与上一帧不衔接，已清空旧历史；不能据此认定 DMA 溢出。";
                    recordPreviewGap(startSample);
                }
                if (parametersChanged)
                    m_previewQualityWarnings << "采样参数变化：旧历史已清空，需重新积累连续数据。";
                if (!m_previewContinuous)
                    m_previewQualityWarnings << "历史连续性待验证：尚未形成连续的预览帧历史。";
            }
            int minA=4095,maxA=0,minB=4095,maxB=0;
            quint32 unusedBits=0;
            QStringList firstWords;
            for (quint32 i = first; i < points; ++i) {
                const quint32 word = readU32LE(m_tcpBuffer, HeaderBytes + int(i * 4u));
                unusedBits |= word & 0xFF000000U;
                if (firstWords.size()<4) firstWords << QString("0x%1").arg(word,8,16,QLatin1Char('0'));
                const int channelA = int(word & 0x0FFFu);
                // ★ AN9238: 12-bit offset binary, 0V 对应 2048, 满量程 -5V~+5V
                const double voltage = (double(channelA) - AdcMidCode) * (AdcVrefMax - AdcVrefMin) / AdcFullScale;
                m_dataBuffer.append(voltage);
                const int channelB = int((word >> 12) & 0x0FFFu);
                minA=qMin(minA,channelA);maxA=qMax(maxA,channelA);
                minB=qMin(minB,channelB);maxB=qMax(maxB,channelB);
                m_dataBufferB.append((double(channelB) - AdcMidCode) * (AdcVrefMax - AdcVrefMin) / AdcFullScale);
            }
            m_inputWarnings.clear();
            QStringList inputDescriptions;
            const auto describe = [this, &inputDescriptions](const QString &channel, int minimum, int maximum) {
                QString signal = QString("%1：原始码 %2～%3").arg(channel).arg(minimum).arg(maximum);
                if (maximum-minimum<=1) {
                    signal += "，无明显交流变化";
                    m_inputWarnings << channel+" 当前预览码值几乎不变，不能据此认为 DAC 正常或无输出。";
                }
                if (minimum<=4 || maximum>=4091) {
                    signal += "，接近量程边界";
                    m_inputWarnings << channel+" 原始码接近 0/4095；请检查输入、接线和 ADC 编码，不要将其当作默认波形。";
                }
                inputDescriptions << signal;
            };
            if (points>first) {describe("A",minA,maxA);describe("B",minB,maxB);}
            else inputDescriptions << "本帧没有预览采样点";
            if (unusedBits) m_inputWarnings << "采样字高 8 位非零，与当前 A[11:0]/B[23:12] 协议约定不符，需核对板端数据格式。";
            m_inputStatus->setText(inputDescriptions.join("\n"));
            m_inputStatus->setStyleSheet(m_inputWarnings.isEmpty()?"color:#234369;":"color:#9a5b12;");
            m_inputDiagnostic=QString("SCP1 预览 seq=%1；原始采样率=%2 Hz；stride=%3；start=%4；帧点数=%5；显示取样=%6\n%7\n"
                "前4个显示采样字：%8；高8位掩码=0x%9\n解包 A=word&0xFFF，B=(word>>12)&0xFFF；"
                "电压=(code-2048)*10/4096 V（沿用原有偏移二进制、10 V跨度约定）。\n%10")
                .arg(seq).arg(rate).arg(stride).arg(startSample).arg(points).arg(points-first)
                .arg(inputDescriptions.join("\n")).arg(firstWords.join(", ")).arg(unusedBits,8,16,QLatin1Char('0'))
                .arg(m_inputWarnings.join("\n"));
            m_inputStatus->setToolTip(m_inputDiagnostic);
            if (m_dataBuffer.size() > DisplayPoints) {
                m_dataBufferB.remove(0, m_dataBuffer.size() - DisplayPoints);
                m_dataBuffer.remove(0, m_dataBuffer.size() - DisplayPoints);
            }
            // Retain a discontinuity warning until the analysis history is replaced
            // with a full buffer of new continuous samples.
            if (m_dataBuffer.size() >= DisplayPoints && !gap && !parametersChanged && first == 0)
                m_previewQualityWarnings.clear();
            updateAcquisitionWarnings();
            const quint64 span = quint64(points) * stride;
            m_previewContinuous = rate > 0 && stride > 0 && points > 0 &&
                startSample <= std::numeric_limits<quint64>::max() - span;
            m_nextPreviewSample = m_previewContinuous ? startSample + span : 0;
            m_previewRate = rate;
            m_previewStride = stride;
            updateWaveform = true;
        }
        if (type == 4 && bytes == 64 && points == 16)
            emit closedLoopStatusReceived(m_tcpBuffer.mid(HeaderBytes, 64));
        if (type == 3) updateAcquisitionStatus(flags, points, seq);
        if (type == 2 && displayRecord(m_tcpBuffer, points, rate, stride, startSample, flags))
            updateWaveform = false; // A coalesced older preview must not cover the record.
        if (type != 1 && type != 2)
            emit protocolStatusFrameReceived(type, flags, seq, points, m_tcpBuffer.mid(HeaderBytes, int(bytes)));
        // Sweep points may arrive rapidly. Log their frame format once per
        // acquisition session; every complete frame is still dispatched above.
        const bool logFrame = type == 1 ? seq % 100 == 0 : type == 3 ? false
            : type == 2 || type == 4 || !m_reportedProtocolTypes.contains(type);
        if (logFrame) {
            m_reportedProtocolTypes.insert(type);
            ui->textEdit_rawData->appendPlainText(
                QString("帧#%1 type=%2 flags=%3 rate=%4Hz points=%5 bytes=%6 start=%7 stride=%8")
                    .arg(seq).arg(type).arg(flags).arg(rate).arg(points).arg(bytes)
                    .arg(startSample).arg(stride));
            if (type == 2)
                ui->textEdit_rawData->appendPlainText(points ? "完整记录已接收并显示为冻结波形；开始/重新布防返回实时采集。" : "收到空记录，原图保留。");
        }
        m_tcpBuffer.remove(0, frameLength);
    }
    if (updateWaveform) m_waveform->setWaveformData(m_dataBuffer, m_dataBufferB, DisplayPoints);
}

void AdcWidget::updatePreviewSamplingLabels()
{
    ui->label_sampleRateValue->setText(formatRate(m_latestPreviewStride
        ? double(m_latestPreviewRate) / m_latestPreviewStride : 0));
    ui->label_sampleRateValue->setToolTip(QString("当前预览波形：原始 %1 Hz / 采样间隔 %2")
        .arg(m_latestPreviewRate).arg(m_latestPreviewStride));
    ui->label_decimationValue->setText(m_latestPreviewStride
        ? "×" + QString::number(m_latestPreviewStride) : "设备未上报");
    ui->label_decimationValue->setToolTip("当前预览波形的采样间隔");
}

bool AdcWidget::displayRecord(const QByteArray &buffer, quint32 points, quint32 rate,
                              quint32 stride, quint64 firstSample, quint32 flags)
{
    if (!points) {
        ui->label_recordStatus->setText("记录：收到空记录，原图保留。"); return false;
    }
    QVector<double> a, b; a.reserve(int(points)); b.reserve(int(points));
    int minA = 4095, maxA = 0, minB = 4095, maxB = 0;
    quint32 unused = 0;
    for (quint32 i = 0; i < points; ++i) {
        const quint32 word = readU32LE(buffer, HeaderBytes + int(i * 4U));
        const int ca = int(word & 0xFFFU), cb = int((word >> 12) & 0xFFFU);
        unused |= word & 0xFF000000U;
        minA = qMin(minA, ca); maxA = qMax(maxA, ca);
        minB = qMin(minB, cb); maxB = qMax(maxB, cb);
        a.append((ca - AdcMidCode) * (AdcVrefMax - AdcVrefMin) / AdcFullScale);
        b.append((cb - AdcMidCode) * (AdcVrefMax - AdcVrefMin) / AdcFullScale);
    }
    const double effectiveRate = stride ? double(rate) / stride : 0.0;
    m_recordDisplayActive = true;
    m_waveform->setRecordedWaveformData(a, b, effectiveRate);
    m_inputWarnings.clear();
    if (unused) m_inputWarnings << "记录采样字高 8 位非零，需核对 A[11:0]/B[23:12] 数据格式。";
    if (!rate || !stride) m_inputWarnings << "记录采样率或采样间隔未知，不能可靠换算时间和频率。";
    if (m_acquisitionOverflowSeen) m_inputWarnings << "本轮 type 3 曾报告 DMA 溢出，请核对记录完整性。";
    m_inputStatus->setText(QString("冻结记录 A：原始码 %1～%2\n冻结记录 B：原始码 %3～%4")
        .arg(minA).arg(maxA).arg(minB).arg(maxB));
    m_inputStatus->setStyleSheet(unused ? "color:#9a5b12;" : "color:#234369;");
    m_inputDiagnostic = QString("SCP1 完整记录：%1 个双通道采样点；start=%2；原始采样率=%3 Hz；stride=%4；flags=0x%5\n"
        "A 原始码 %6～%7；B 原始码 %8～%9；高8位掩码=0x%10\n"
        "电压=(code-2048)*10/4096 V；原图显示冻结记录，缩放后从原始点重新绘制包络。\n%11")
        .arg(points).arg(firstSample).arg(rate).arg(stride).arg(flags,8,16,QLatin1Char('0'))
        .arg(minA).arg(maxA).arg(minB).arg(maxB).arg(unused,8,16,QLatin1Char('0')).arg(m_inputWarnings.join("\n"));
    m_inputStatus->setToolTip(m_inputDiagnostic);
    QStringList quality = m_inputWarnings;
    quality << "当前分析对象为已冻结的完整记录，后续实时预览不会覆盖。";
    m_waveform->setAcquisitionWarnings(quality);
    const QString duration = effectiveRate > 0 ? QString("，约 %1 ms").arg(points * 1000.0 / effectiveRate,0,'g',7) : QString();
    ui->label_recordStatus->setText(QString("记录：已接收 %1 个双通道采样点%2，已显示冻结波形。缩放或光标查看；开始/重新布防返回实时。")
        .arg(points).arg(duration));
    ui->label_recordStatus->setToolTip(m_inputDiagnostic);
    ui->label_sampleRateValue->setText(formatRate(effectiveRate));
    ui->label_sampleRateValue->setToolTip(QString("当前显示完整记录：原始 %1 Hz / 采样间隔 %2").arg(rate).arg(stride));
    ui->label_decimationValue->setText(stride ? "×" + QString::number(stride) : "设备未上报");
    ui->label_decimationValue->setToolTip("冻结记录的采样间隔");
    return true;
}

void AdcWidget::readUdpData()
{
    while (m_udpSocket->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(int(m_udpSocket->pendingDatagramSize()));
        m_udpSocket->readDatagram(data.data(), data.size());
    }
}
