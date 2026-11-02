#ifndef ADCWIDGET_H
#define ADCWIDGET_H

#include <QWidget>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QHostAddress>
#include <QVector>
#include <QTimer>
#include <QStringList>
#include <QSet>
#include <QPointer>
#include "experiment_analysis.h"
#include <functional>

class WaveformWidget;
class QDialog;
class QLabel;

namespace Ui {
class AdcWidget;
}

class AdcWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AdcWidget(QWidget *parent = nullptr);
    ~AdcWidget();
    QTcpSocket *sharedSocket() const { return m_tcpSocket; }
    bool connectionRequested() const { return m_wantConnection; }
    QString connectionIp() const;
    quint16 connectionPort() const;
    void toggleSharedConnection(const QString &ip, quint16 port);
    void setExperimentAnalysisSource(const std::function<ExperimentAnalysis::Snapshot()> &source);

signals:
    void requestBack();
    void requestClosedLoopControl();
    void sharedConnectionChanged();
    void tcpBytesReceived(const QByteArray &bytes);
    void closedLoopStatusReceived(const QByteArray &payload);
    // Complete non-sample frames; preserve flags for PLL and other future handlers.
    void protocolStatusFrameReceived(quint16 type, quint32 flags, quint32 sequence,
                                     quint32 count, const QByteArray &payload);

private slots:
    void on_btn_connect_clicked();
    void on_btn_pause_clicked();
    void on_btn_arm_clicked();       // ARM 命令
    void on_btn_trigger_clicked();   // TRIGGER 命令
    void on_action_dump_triggered(); // DUMP 命令
    void on_action_windowSettings_triggered();
    void readUdpData();
    void readTcpData();
   // void generateMockData();

private:
    Ui::AdcWidget *ui;
    WaveformWidget *m_waveform = nullptr;
    QTcpSocket *m_tcpSocket;
    QUdpSocket *m_udpSocket;
    //QTimer *m_mockTimer;
    //double m_mockPhase;
    bool m_isConnected;
    bool m_acquisitionStatusKnown = false;
    quint32 m_acquisitionFlags = 0;
    bool m_acquisitionOverflowSeen = false;
    QPointer<QDialog> m_windowSettingsDialog;
    quint32 m_windowViewUs = 10000;
    quint32 m_windowBeforeUs = 2000;
    quint32 m_windowAfterUs = 2000;
    bool m_wantConnection = false;
    QTimer *m_reconnectTimer = nullptr;
    QHostAddress m_targetAddress;
    quint16 m_targetPort = 0;
    bool m_previewContinuous = false;
    quint64 m_nextPreviewSample = 0;
    quint32 m_previewRate = 0;
    quint32 m_previewStride = 0;
    quint32 m_reportedPreviewRate = 0;
    quint32 m_reportedPreviewStride = 0;
    quint32 m_latestPreviewRate = 0;
    quint32 m_latestPreviewStride = 0;
    QStringList m_previewQualityWarnings;
    QLabel *m_inputStatus = nullptr;
    QString m_inputDiagnostic;
    QStringList m_inputWarnings;
    QSet<quint16> m_reportedProtocolTypes;
    QTimer *m_gapLogTimer = nullptr;
    quint64 m_gapCount = 0;
    quint64 m_pendingGapCount = 0;
    quint64 m_lastGapExpected = 0;
    quint64 m_lastGapActual = 0;

    QVector<double> m_dataBuffer;
    QVector<double> m_dataBufferB;
    QByteArray m_tcpBuffer;  // ★ TCP 累积缓冲区
    bool m_recordDisplayActive = false;
    quint64 m_discardRecordBytes = 0;

    // 辅助：小端解析
    quint16 readU16LE(const QByteArray &buf, int offset);
    quint32 readU32LE(const QByteArray &buf, int offset);
    quint64 readU64LE(const QByteArray &buf, int offset);

    bool sendCommand(const QString &cmd);  // True means queued, not acknowledged.
    void resetSamplingDisplay(bool clearRecord = false);
    bool displayRecord(const QByteArray &buffer, quint32 points, quint32 rate,
                       quint32 stride, quint64 firstSample, quint32 flags);
    void recordPreviewGap(quint64 actualStart);
    void flushPreviewGapLog();
    void updateAcquisitionStatus(quint32 flags, quint32 count, quint32 sequence);
    void updateAcquisitionWarnings();
    void updatePreviewSamplingLabels();
    void parseTcpFrame(const QByteArray &frame);  // 解析一帧
};

#endif // ADCWIDGET_H
