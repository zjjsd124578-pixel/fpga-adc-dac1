#ifndef DACWIDGET_H
#define DACWIDGET_H
#include <QWidget>
#include <QStringList>
#include <QByteArray>
#include <QPointer>
#include <QTcpSocket>
#include "experiment_analysis.h"
class AdcWidget;
class QTcpSocket;
class QTimer;
class QLabel;
class QPushButton;
class QGroupBox;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QPlainTextEdit;
class QBoxLayout;
class CircuitTestPanel;
class QDialog;
namespace Ui { class DacWidget; }
class DacWidget : public QWidget
{
    Q_OBJECT
public:
    explicit DacWidget(QWidget *parent = nullptr, AdcWidget *controller = nullptr);
    ~DacWidget();
    void selectAmplitudeLoopMode();
    void setAmplitudeLoopMode(bool enabled);
    void setSweepControlsVisible(bool visible);
    void setDigitalPllMode(bool enabled);
    void setCircuitTestMode(bool enabled);
    ExperimentAnalysis::Snapshot experimentSnapshot() const;
protected:
    void resizeEvent(QResizeEvent *event) override;
signals:
    void requestBack();
private:
    void toggleConnection();
    void sendFixedWave();
    bool usesTargetAmplitude() const;
    void refreshAmplitudeMode();
    bool dcOffsetCommandMillivolts(int &millivolts) const;
    void startSweep();
    bool sendCommands(const QStringList &commands, bool logRequest = true);
    void updateReceiveStatistics();
    void observeReceivedBytes(const QByteArray &bytes);
    void refreshConnectionState();
    void showLoopStatus(const QByteArray &payload);
    void pollLoopProcess();
    ExperimentAnalysis::History m_analysisHistory;
    QCheckBox *m_loopRecording=nullptr;
    QTimer *m_loopRefreshTimer=nullptr,*m_loopReplyTimer=nullptr;
    bool m_loopQueryPending=false;
    void createPllControls();
    void startDigitalPll();
    void queryDigitalPll(bool logRequest = true);
    void showPllStatus(quint32 flags, const QByteArray &payload);
    QGroupBox *m_pllPanel;
    QGroupBox *m_pllSettings;
    QBoxLayout *m_pllColumns;
    QSpinBox *m_pllCenter, *m_pllHysteresis, *m_pllMinHz, *m_pllMaxHz, *m_pllKp, *m_pllKi;
    QDoubleSpinBox *m_pllAmplitude;
    QComboBox *m_pllFormat;
    QCheckBox *m_pllAutoQuery;
    QLabel *m_pllStatus;
    QPlainTextEdit *m_pllDetails;
    QPushButton *m_pllStart, *m_pllStop, *m_pllQuery, *m_pllClose;
    QTimer *m_pllRefreshTimer, *m_pllReplyTimer;
    bool m_digitalPllMode = false;
    bool m_pllQueryPending = false;
    bool m_pllManualQuery = false;
    bool m_pllQueryObsolete = false;
    void beginOutputChange(const QStringList &commands, const QString &resultText,
                           bool stopPll = true);
    void beginLoopStop();
    void beginPllStop();
    void pollModeStatus();
    void modeConfirmationTimedOut();
    void finishOutputChange();
    void cancelOutputChange(const QString &reason);
    void handleModeStatus(quint16 type, quint32 flags, quint32 sequence,
                          quint32 count, const QByteArray &payload);
    void parseStandaloneFrames(const QByteArray &bytes);
    enum class ModeStep { Idle, StopTask, StopManual, StopPll };
    ModeStep m_modeStep = ModeStep::Idle;
    QTimer *m_modePollTimer;
    QTimer *m_modeDeadline;
    QLabel *m_modeStatus;
    QString m_lastModeReply;
    QStringList m_pendingOutput;
    QString m_pendingResult;
    QByteArray m_statusBuffer;
    bool m_stopPll = true;
    bool m_pllStopSent = false;
    bool m_autoTaskActive = false;
    bool m_manualLoopPossible = false;
    bool m_pllMayBeActive = false;
    bool m_queryPending = false;
    int m_expectedTaskStage = -1;
    Ui::DacWidget *ui;
    QPointer<QTcpSocket> m_socket;
    QPointer<AdcWidget> m_controller;
    QTimer *m_connectTimer;
    QTimer *m_receiveTimer;
    quint64 m_receivedBytes = 0;
    quint64 m_receivedChunks = 0;
    QByteArray m_hexPreview;
    bool m_hasNewData = false;
    bool m_amplitudeLoopMode = false;
    CircuitTestPanel *m_circuitPanel;
    bool m_circuitTestMode = false;
};
#endif
