#ifndef WAVEFORMWIDGET_H
#define WAVEFORMWIDGET_H

#include <QWidget>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include <QVector>
#include <QPointer>
#include <QStringList>
#include <QElapsedTimer>
#include "experiment_analysis.h"
#include <functional>

class QLabel;
class QPushButton;
class QDialog;
class QCheckBox;
class QComboBox;
class QTimer;
class QNetworkAccessManager;

QT_CHARTS_USE_NAMESPACE

class WaveformWidget : public QWidget
{
    Q_OBJECT
public:
    explicit WaveformWidget(QWidget *parent = nullptr, QNetworkAccessManager *aiNetwork = nullptr);

    void setWaveformData(const QVector<double> &data, int maxPoints = 3000);
    void setWaveformData(const QVector<double> &channelA, const QVector<double> &channelB, int maxPoints = 3000);
    void setRecordedWaveformData(const QVector<double> &channelA, const QVector<double> &channelB, double sampleRateHz);
    bool isShowingRecord() const { return m_recordMode; }
    void setDisplayPaused(bool paused);
    bool isDisplayPaused() const { return m_displayPaused; }
    bool isHoldingDisplay() const { return m_displayPaused || m_cursorMode != NoCursor; }
    bool canPauseDisplay() const { return !m_recordMode && !m_visibleData.isEmpty(); }
    void resetZoom();
    void autoFit();
    void setSampleRateHz(double rate);
    void autoMeasureFrequency();
    void showSpectrum();
    void analyzeWithAi();
    void setExperimentAnalysisSource(const std::function<ExperimentAnalysis::Snapshot()> &source){m_experimentSource=source;}
    void exportWaveform();
    void setAcquisitionWarnings(const QStringList &warnings) {
        m_latestAcquisitionWarnings = warnings;
        if (m_recordMode) m_visibleAcquisitionWarnings = warnings;
    }

    void setAutoScroll(bool enable) { m_autoScroll = enable; }
    bool isAutoScroll() const { return m_autoScroll; }

signals:
    void requestClosedLoopControl();
    void displayStateChanged();

protected:
    void hideEvent(QHideEvent *event) override;
    // ★ 用事件过滤器接管 QChartView 的鼠标事件
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QPointer<QNetworkAccessManager> m_aiNetwork;
    std::function<ExperimentAnalysis::Snapshot()> m_experimentSource;
    QPointer<QDialog> m_aiDialog;
    enum CursorMode { NoCursor, TimeCursor, VoltageCursor };
    void selectCursorMode(CursorMode mode);
    void clearCursors();
    void updateCursorLines();
    void updateMeasurements();
    void updateStatistics();
    void updateFrequencyDisplay();
    void renderRecord();
    void fitRecordVoltage();
    const QVector<double> &rawAnalysisData(int channel, bool latest) const;
    QVector<double> analysisData(int channel, bool latest) const;
    QString analysisChannelName(int channel) const;
    QString probeDescription(int channel) const;
    void changeProbeScale(int channel);
    void renderPreview();
    double sampleToAxis(double sample) const;
    double axisToSample(double coordinate) const;
    void setSampleRange(double first, double last);
    double m_xUnitsPerSample = 1.0;
    bool m_updatingTimeAxis = false;
    double m_probeScale[2] = {1.0, 1.0};
    QComboBox *m_probeSelectors[2] = {nullptr, nullptr};
    int m_previewMaxPoints = 3000;

    QChart       *m_chart;
    QChartView   *m_chartView;
    QLineSeries  *m_series;
    QLineSeries  *m_seriesB;
    QCheckBox *m_showA;
    QCheckBox *m_showB;
    int m_spectrumChannelIndex = 0;
    int m_aiChannelIndex = 0;
    QValueAxis   *m_axisX;
    QValueAxis   *m_axisY;
    bool          m_autoScroll = true;
    bool          m_autoFitY = false;
    bool          m_recordMode = false;
    bool          m_displayPaused = false;
    QVector<double> m_lastData;
    QVector<double> m_visibleData;
    QVector<double> m_lastDataB;
    QVector<double> m_visibleDataB;
    QStringList m_latestAcquisitionWarnings;
    QStringList m_visibleAcquisitionWarnings;
    double m_sampleRateHz = 0;
    double m_latestSampleRateHz = 0;
    CursorMode m_cursorMode = NoCursor;
    QLineSeries *m_timeCursorLines[2] = {nullptr, nullptr};
    QLineSeries *m_voltageCursorLines[2] = {nullptr, nullptr};
    bool m_hasTimeCursor[2] = {false, false};
    bool m_hasVoltageCursor[2] = {false, false};
    double m_timeCursorX[2] = {0.0, 0.0};
    double m_voltageCursorY[2] = {0.0, 0.0};
    QLabel *m_measureLabel = nullptr;
    QLabel *m_autoMeasureLabel = nullptr;
    QPushButton *m_frequencyButton = nullptr;
    QTimer *m_frequencyTimer = nullptr;
    QElapsedTimer m_frequencyDataAge;
    QStringList m_frequencyResults;
    bool m_frequencyDirty = false;
    QLabel *m_aiStatusLabel = nullptr;
    QLabel *m_statisticsLabel = nullptr;
    QPushButton *m_timeButton = nullptr;
    QPushButton *m_voltageButton = nullptr;
    QPointer<QDialog> m_spectrumDialog;
    QPointer<QDialog> m_exportDialog;
    QString m_lastExportDirectory;

    bool          m_isDragging = false;
    QPoint        m_lastMousePos;
};

#endif // WAVEFORMWIDGET_H
