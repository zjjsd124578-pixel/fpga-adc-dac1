#include "waveformwidget.h"
#include "frequency_estimator.h"
#include "waveform_statistics.h"
#include "spectrum_analyzer.h"
#include "ai_api.h"
#include "waveform_quality.h"
#include "waveform_ai_report.h"
#include "experiment_ai_dialog.h"
#include <QStackedWidget>
#include <QtCharts/QChart>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QPen>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <limits>
#include <QStringList>
#include <QDialog>
#include <QtCharts/QScatterSeries>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSslSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QTimer>
#include <QComboBox>
#include <QHideEvent>
#include <QFrame>
#include <QGridLayout>
#include <QTextEdit>
#include <QProgressBar>
#include <QApplication>
#include <QClipboard>
#include <QTabWidget>
#include <QCheckBox>
#include <QGraphicsLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QSharedPointer>
#include <QPainter>

namespace {
const double DefaultYMin = -5.0;
const double DefaultYMax = 5.0;
const double MinYSpan = 0.001; // 1 mV，避免无限放大后坐标轴失效。
const double MaxYSpan = 100.0;

QVector<double> probeScaled(const QVector<double> &raw, double scale)
{
    QVector<double> values = raw;
    if (scale != 1.0) for (double &v : values) v *= scale;
    return values;
}

struct AiWaveformSnapshot {
    QVector<double> data;
    QString channelName;
    WaveformStatistics stats;
    FrequencyEstimate frequency;
    SpectrumResult spectrum;
    QStringList quality;
    WaveformAi::Evidence evidence;
    double probeScale = 1.0;
};

struct WaveformExportSnapshot {
    QVector<double> a, b;
    double sampleRateHz = 0, xMin = 0, xMax = 0, probeA = 1.0, probeB = 1.0;
    QDateTime selectedAt;
    QString source;
    QStringList notes;
    QImage image;
};

QPair<int,int> exportSampleRange(const WaveformExportSnapshot &snapshot, bool all)
{
    const int count = std::max(snapshot.a.size(), snapshot.b.size());
    if (!count) return qMakePair(-1,-1);
    if (all) return qMakePair(0,count-1);
    if (!std::isfinite(snapshot.xMin) || !std::isfinite(snapshot.xMax)
        || snapshot.xMax < 0 || snapshot.xMin > count-1) return qMakePair(-1,-1);
    const int first = int(std::ceil(std::max(0.0,snapshot.xMin)));
    const int last = int(std::floor(std::min(double(count-1),snapshot.xMax)));
    return first <= last ? qMakePair(first,last) : qMakePair(-1,-1);
}

QByteArray exportNumber(double value)
{
    return std::isfinite(value) ? QByteArray::number(value,'g',17) : QByteArray();
}

struct WaveformExportJob {
    QSharedPointer<QSaveFile> file;
    int first = 0, last = -1, next = 0;
    bool active = false;
    QString path;
};

// Preserve short pulses in a record overview. Zooming rebuilds the envelope
// from the retained full-resolution samples, rather than magnifying old bins.
QVector<QPointF> recordEnvelope(const QVector<double> &values, int first, int last)
{
    QVector<QPointF> result;
    if (values.isEmpty() || first > last) return result;
    const int bucket = std::max(1, (last - first + 1500) / 1500);
    result.reserve(3004);
    result.append(QPointF(first, values[first]));
    for (int start = first; start <= last; start += bucket) {
        const int end = std::min(last, start + bucket - 1);
        int low = start, high = start;
        for (int i = start + 1; i <= end; ++i) {
            if (values[i] < values[low]) low = i;
            if (values[i] > values[high]) high = i;
        }
        const int a = std::min(low, high), b = std::max(low, high);
        if (result.last().x() != a) result.append(QPointF(a, values[a]));
        if (b != a) result.append(QPointF(b, values[b]));
    }
    if (result.last().x() != last) result.append(QPointF(last, values[last]));
    return result;
}
}

WaveformWidget::WaveformWidget(QWidget *parent, QNetworkAccessManager *aiNetwork)
    : QWidget(parent), m_aiNetwork(aiNetwork)
{
    m_chart = new QChart();
    m_chart->setTitle("实时波形（滚轮：横轴；Shift+滚轮：纵轴；右键：复位）");
    m_chart->legend()->hide();
    m_chart->setAnimationOptions(QChart::NoAnimation);
    m_chart->setBackgroundRoundness(10);
    m_chart->setTitleBrush(QColor("#526780"));
    m_chart->setTitleFont(QFont("Microsoft YaHei", 10));

    m_series = new QLineSeries();
    m_series->setName("A 通道");
    m_series->setObjectName("waveformSeriesA");
    m_series->setPen(QPen(QColor("#168bd2"), 1.3));
    m_chart->addSeries(m_series);
    m_seriesB = new QLineSeries();
    m_seriesB->setName("B 通道");
    m_seriesB->setObjectName("waveformSeriesB");
    m_seriesB->setPen(QPen(QColor("#e58a23"), 1.3));
    m_chart->addSeries(m_seriesB);

    m_axisX = new QValueAxis();
    m_axisX->setRange(0, 1000);
    m_axisX->setTitleText("采样点（采样率未知）");
    m_axisX->setTickCount(11);
    m_axisX->setLabelFormat("%.0f");

    // ★ AN9238 输入范围 -5V ~ +5V
    m_axisY = new QValueAxis();
    m_axisY->setRange(DefaultYMin, DefaultYMax);
    m_axisY->setTitleText("电压 (V)");
    m_axisY->setTickCount(6);

    m_chart->addAxis(m_axisX, Qt::AlignBottom);
    m_chart->addAxis(m_axisY, Qt::AlignLeft);
    for (QValueAxis *axis : {m_axisX, m_axisY}) {
        axis->setLabelsFont(QFont("Microsoft YaHei", 9));
        axis->setTitleFont(QFont("Microsoft YaHei", 10));
        axis->setLabelsColor(QColor("#526780"));
        axis->setGridLineColor(QColor("#e5ebf3"));
        axis->setLinePenColor(QColor("#cfd9e6"));
    }
    m_series->attachAxis(m_axisX);
    m_series->attachAxis(m_axisY);
    m_seriesB->attachAxis(m_axisX);
    m_seriesB->attachAxis(m_axisY);

    for (int i = 0; i < 2; ++i) {
        m_timeCursorLines[i] = new QLineSeries();
        m_voltageCursorLines[i] = new QLineSeries();
        m_timeCursorLines[i]->setPen(QPen(i == 0 ? QColor("#e59a1f") : QColor("#d34d2c"), 2));
        m_voltageCursorLines[i]->setPen(QPen(i == 0 ? QColor("#19a7a0") : QColor("#6554ce"), 2));
        for (QLineSeries *cursor : {m_timeCursorLines[i], m_voltageCursorLines[i]}) {
            m_chart->addSeries(cursor);
            cursor->attachAxis(m_axisX);
            cursor->attachAxis(m_axisY);
        }
    }
    connect(m_axisX, &QValueAxis::rangeChanged, this, [this]() {
        if (!m_updatingTimeAxis) setSampleRange(axisToSample(m_axisX->min()), axisToSample(m_axisX->max()));
    });
    connect(m_axisY, &QValueAxis::rangeChanged, this, &WaveformWidget::updateCursorLines);

    m_chartView = new QChartView(m_chart);
    m_chart->layout()->setMinimumSize(0, 0);
    m_chart->setMinimumSize(0, 0);
    m_chartView->setMinimumSize(0, 260);
    m_chartView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_chartView->setStyleSheet("background: transparent; border: none;");
    m_chartView->setMouseTracking(true);
    m_chartView->setRenderHint(QPainter::Antialiasing, false);
    m_chartView->installEventFilter(this);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    QHBoxLayout *toolbar = new QHBoxLayout();
    QPushButton *autoButton = new QPushButton("自动调整波形", this);
    autoButton->setToolTip("实时模式按最近 1000 点调整并跟随；暂停波形或查看记录时，按保留的数据调整范围。");
    toolbar->addWidget(autoButton);
    m_timeButton = new QPushButton("时间光标", this);
    m_voltageButton = new QPushButton("电压光标", this);
    QPushButton *clearButton = new QPushButton("清除光标", this);
    m_frequencyButton = new QPushButton("实时测频", this);
    QPushButton *measureButton = m_frequencyButton;
    measureButton->setObjectName("btn_liveFrequency");
    measureButton->setCheckable(true);
    measureButton->setChecked(true);
    measureButton->setToolTip("实时模式每 200 ms 测量最新采样；暂停波形后测量保留的波形；记录模式测量采集记录。点击可暂停或继续测频。");
    QPushButton *spectrumButton = new QPushButton("FFT 频谱", this);
    QPushButton *aiButton = new QPushButton("AI 分析", this);
    QPushButton *exportButton = new QPushButton("数据导出", this);
    exportButton->setObjectName("btn_dataExport");
    exportButton->setEnabled(false);
    exportButton->setToolTip("保存两路波形数据（CSV）或当前波形图片（PNG）；板子继续采集。");
    m_timeButton->setCheckable(true);
    m_voltageButton->setCheckable(true);
    toolbar->addWidget(m_timeButton);
    toolbar->addWidget(m_voltageButton);
    toolbar->addWidget(clearButton);
    toolbar->addWidget(measureButton);
    toolbar->addWidget(spectrumButton);
    toolbar->addWidget(aiButton);
    toolbar->addWidget(exportButton);
    toolbar->addStretch();
    layout->addLayout(toolbar);
    QHBoxLayout *channels = new QHBoxLayout();
    channels->addWidget(new QLabel("通道 / 探头倍率：", this));
    m_showA = new QCheckBox("A 通道", this);
    m_showA->setObjectName("checkBox_showA");
    m_showA->setStyleSheet("color: #168bd2;");
    m_showB = new QCheckBox("B 通道", this);
    m_showB->setObjectName("checkBox_showB");
    m_showB->setStyleSheet("color: #c57316;");
    m_showA->setChecked(true);
    m_showB->setChecked(true);
    for (int channel = 0; channel < 2; ++channel) {
        channels->addWidget(channel == 0 ? m_showA : m_showB);
        auto selector = new QComboBox(this); m_probeSelectors[channel] = selector;
        selector->setObjectName(channel == 0 ? "comboBox_probeA" : "comboBox_probeB");
        selector->addItem("×1/2", 0.5); selector->addItem("×1", 1.0); selector->addItem("×2", 2.0);
        selector->setCurrentIndex(1); selector->setMinimumWidth(78);
        selector->setAccessibleName(channel == 0 ? "A 通道探头倍率" : "B 通道探头倍率");
        selector->setToolTip("探头倍率：显示电压 = ADC 换算电压 × 倍率。同步用于统计、FFT、波形 AI 和导出；频率与时间不变。只在本次程序中生效，不改变板端采集、DAC、闭环或电路自动测试。");
        channels->addWidget(selector);
        connect(selector,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,channel](int){changeProbeScale(channel);});
        if (channel == 0) channels->addSpacing(12);
    }
    channels->addStretch();
    layout->addLayout(channels);
    const auto visibilityChanged = [this, measureButton]() {
        m_series->setVisible(m_showA->isChecked());
        m_seriesB->setVisible(m_showB->isChecked());
        updateStatistics();
        if (m_frequencyButton->isChecked()) autoMeasureFrequency();
        else {
            m_frequencyResults.clear();
            updateFrequencyDisplay();
        }
        const bool anyVisible = m_showA->isChecked() || m_showB->isChecked();
        measureButton->setEnabled(anyVisible);
        if (m_recordMode) renderRecord();
        else if (m_autoFitY && m_displayPaused) fitRecordVoltage();
        else if (m_autoFitY && m_cursorMode == NoCursor && !m_lastData.isEmpty())
            setWaveformData(m_lastData, m_lastDataB, 3000);
    };
    connect(m_showA, &QCheckBox::toggled, this, visibilityChanged);
    connect(m_showB, &QCheckBox::toggled, this, visibilityChanged);
    layout->addWidget(m_chartView, 1);
    m_measureLabel = new QLabel("光标测量：选择时间或电压光标，在波形区域点击两次。", this);
    m_measureLabel->setWordWrap(true);
    layout->addWidget(m_measureLabel);
    m_autoMeasureLabel = new QLabel("实时测频：等待采集数据。", this);
    m_autoMeasureLabel->setObjectName("label_autoMeasure");
    m_autoMeasureLabel->setWordWrap(true);
    layout->addWidget(m_autoMeasureLabel);
    m_statisticsLabel = new QLabel("波形统计（当前窗口）：\nA 通道：暂无数据。\nB 通道：暂无数据。", this);
    m_statisticsLabel->setObjectName("label_waveformStatistics");
    m_statisticsLabel->setWordWrap(true);
    layout->addWidget(m_statisticsLabel);
    m_aiStatusLabel = new QLabel("AI 分析：点击按钮分析当前波形。", this);
    m_aiStatusLabel->setObjectName("label_aiStatus");
    m_aiStatusLabel->setWordWrap(true);
    layout->addWidget(m_aiStatusLabel);
    setLayout(layout);
    connect(autoButton, &QPushButton::clicked, this, &WaveformWidget::autoFit);
    connect(m_timeButton, &QPushButton::clicked, this,
            [this]() { selectCursorMode(TimeCursor); });
    connect(m_voltageButton, &QPushButton::clicked, this,
            [this]() { selectCursorMode(VoltageCursor); });
    connect(clearButton, &QPushButton::clicked, this, &WaveformWidget::clearCursors);
    connect(measureButton, &QPushButton::toggled, this, [this](bool enabled) {
        m_frequencyButton->setText(enabled ? (m_recordMode ? "记录测频" : "实时测频") : "继续测频");
        if (enabled) autoMeasureFrequency(); else updateFrequencyDisplay();
    });
    m_frequencyTimer = new QTimer(this);
    m_frequencyTimer->setInterval(200);
    connect(m_frequencyTimer, &QTimer::timeout, this, [this]() {
        if (!isVisible() || !m_frequencyButton->isChecked()) return;
        if (m_frequencyDirty) autoMeasureFrequency(); else updateFrequencyDisplay();
    });
    m_frequencyTimer->start();
    connect(spectrumButton, &QPushButton::clicked, this, &WaveformWidget::showSpectrum);
    connect(aiButton, &QPushButton::clicked, this, &WaveformWidget::analyzeWithAi);
    connect(exportButton, &QPushButton::clicked, this, &WaveformWidget::exportWaveform);
    connect(this, &WaveformWidget::displayStateChanged, this, [=]() {
        exportButton->setEnabled(!m_visibleData.isEmpty() || !m_visibleDataB.isEmpty());
    });
}

void WaveformWidget::exportWaveform()
{
    if (m_exportDialog) {
        m_exportDialog->show(); m_exportDialog->raise(); m_exportDialog->activateWindow();
        return;
    }
    if (m_visibleData.isEmpty() && m_visibleDataB.isEmpty()) return;
    WaveformExportSnapshot snapshot;
    snapshot.a = analysisData(0, false); snapshot.b = analysisData(1, false);
    snapshot.probeA = m_probeScale[0]; snapshot.probeB = m_probeScale[1];
    snapshot.sampleRateHz = m_sampleRateHz;
    snapshot.xMin = axisToSample(m_axisX->min()); snapshot.xMax = axisToSample(m_axisX->max());
    snapshot.selectedAt = QDateTime::currentDateTime();
    snapshot.source = m_recordMode ? "采集记录" : m_displayPaused ? "已暂停波形"
        : m_cursorMode != NoCursor ? "光标测量波形" : "实时预览";
    snapshot.notes = m_visibleAcquisitionWarnings;
    const auto visibleRange = exportSampleRange(snapshot,false);
    const int visibleCount = visibleRange.first >= 0 ? visibleRange.second-visibleRange.first+1 : 0;
    const QPixmap plot = m_chartView->grab();
    const qreal dpr = plot.devicePixelRatio();
    const QSize size = plot.size()/dpr;
    const int headingHeight = 76;
    snapshot.image = QImage(QSize(qRound(size.width()*dpr),qRound((size.height()+headingHeight)*dpr)),QImage::Format_ARGB32_Premultiplied);
    snapshot.image.setDevicePixelRatio(dpr);
    snapshot.image.fill(Qt::white);
    {
        QPainter painter(&snapshot.image);
        painter.setPen(QColor("#234369"));
        painter.setFont(QFont("Microsoft YaHei",11,QFont::Bold));
        painter.drawText(12,22,"ADC 波形 · " + snapshot.source);
        painter.setFont(QFont("Microsoft YaHei",9));
        painter.drawText(12,43,QString("采样率：%1；画面范围内 %2 点；探头 A %3，B %4")
            .arg(snapshot.sampleRateHz>0 ? QString::number(snapshot.sampleRateHz,'g',10)+" Sa/s" : "未知").arg(visibleCount).arg(probeDescription(0),probeDescription(1)));
        painter.setPen(QColor("#69788d"));
        painter.drawText(12,64,"画面选取时间（电脑）："+snapshot.selectedAt.toString("yyyy-MM-dd HH:mm:ss.zzz"));
        for(int channel=0;channel<2;++channel) {
            if (!(channel==0 ? m_showA->isChecked() : m_showB->isChecked())) continue;
            const int x=size.width()-180+channel*86;
            painter.setPen(QPen(channel==0 ? QColor("#168bd2") : QColor("#e58a23"),2));
            painter.drawLine(x,18,x+18,18);
            painter.drawText(x+22,22,analysisChannelName(channel));
        }
        painter.drawPixmap(0,headingHeight,plot);
    }

    QDialog *dialog=new QDialog(this);
    m_exportDialog=dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("waveformExportDialog");
    dialog->setWindowTitle("数据导出");
    dialog->resize(570,320);
    auto layout=new QVBoxLayout(dialog);
    layout->setContentsMargins(20,18,20,18); layout->setSpacing(12);
    auto info=new QLabel(QString("已保留打开窗口时的波形，板子继续采集。\n数据来源：%1；共 %2 个采样点。")
        .arg(snapshot.source).arg(std::max(snapshot.a.size(),snapshot.b.size())),dialog);
    info->setText(info->text()+"\n探头倍率：A "+probeDescription(0)+"，B "+probeDescription(1)+"；电压已按倍率换算。");
    info->setWordWrap(true); info->setObjectName("label_exportSource"); layout->addWidget(info);
    auto form=new QFormLayout();
    auto format=new QComboBox(dialog); format->setObjectName("comboBox_exportFormat");
    format->addItems({"CSV 数据（.csv）","PNG 波形图片（.png）"});
    auto scope=new QComboBox(dialog); scope->setObjectName("comboBox_exportRange");
    scope->addItems({"当前画面范围","全部已接收数据"});
    scope->setToolTip("全部已接收数据指当前保留的预览数据或已接收的采集记录。");
    form->addRow("保存格式",format); form->addRow("数据范围",scope); layout->addLayout(form);
    auto summary=new QLabel(dialog); summary->setWordWrap(true); summary->setObjectName("label_exportSummary"); layout->addWidget(summary);
    auto hint=new QLabel("CSV 保存两路电压及时间；PNG 按当前图中显示保存。时间从保留数据的第 0 点算起，不代表触发时刻。",dialog);
    hint->setWordWrap(true); layout->addWidget(hint);
    auto status=new QLabel(dialog); status->setWordWrap(true); status->setTextFormat(Qt::PlainText);
    status->setObjectName("label_exportStatus"); layout->addWidget(status);
    auto progress=new QProgressBar(dialog); progress->setObjectName("progress_export"); progress->hide(); layout->addWidget(progress);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Close,dialog);
    buttons->button(QDialogButtonBox::Close)->setText("关闭");
    auto save=buttons->addButton("保存…",QDialogButtonBox::ActionRole); save->setObjectName("btn_exportSave"); save->setDefault(true);
    auto cancel=buttons->addButton("取消保存",QDialogButtonBox::ActionRole); cancel->setObjectName("btn_exportCancel"); cancel->hide();
    layout->addWidget(buttons);
    auto state=QSharedPointer<WaveformExportJob>::create();
    auto timer=new QTimer(dialog); timer->setInterval(1);
    const auto refresh=[=]() {
        const bool csv=format->currentIndex()==0;
        const auto range=exportSampleRange(snapshot,scope->currentIndex()==1);
        const int count=range.first>=0 ? range.second-range.first+1 : 0;
        scope->setEnabled(csv && !state->active);
        save->setEnabled(!state->active && (!csv || count>0));
        QString text=csv ? QString("将保存 %1 个采样点，A 通道和 B 通道各一列。") .arg(count)
            : "将保存当前波形画面、坐标轴和光标，并标注数据来源与采样率。";
        if(csv && snapshot.sampleRateHz<=0)text+="\n采样率未知，时间列留空；电压和采样点序号仍可保存。";
        if(csv && count==0)text+="\n当前画面范围内没有采样点，请选择全部已接收数据。";
        summary->setText(text);
    };
    const auto finish=[=](bool success,const QString &message) {
        timer->stop(); state->active=false;
        if(state->file) {if(!success)state->file->cancelWriting();state->file.clear();}
        format->setEnabled(true); cancel->hide(); progress->hide(); refresh();
        status->setStyleSheet(success ? "color:#20805b;" : "color:#9a5b12;"); status->setText(message);
    };
    connect(cancel,&QPushButton::clicked,dialog,[=]() {finish(false,"已取消保存，文件未改动。");});
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::close);
    connect(dialog,&QDialog::rejected,dialog,[=]() {if(state->active)finish(false,"已取消保存。");});
    connect(format,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[=](int) {refresh();});
    connect(scope,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[=](int) {refresh();});
    connect(timer,&QTimer::timeout,dialog,[=]() {
        if(!state->active || !state->file)return;
        const int end=std::min(state->last+1,state->next+4096);
        QByteArray chunk; chunk.reserve((end-state->next)*100);
        for(int i=state->next;i<end;++i) {
            chunk+=QByteArray::number(i); chunk+=',';
            if(snapshot.sampleRateHz>0)chunk+=exportNumber(double(i)/snapshot.sampleRateHz);
            chunk+=','; if(i<snapshot.a.size())chunk+=exportNumber(snapshot.a[i]);
            chunk+=','; if(i<snapshot.b.size())chunk+=exportNumber(snapshot.b[i]);
            chunk+='\n';
        }
        if(state->file->write(chunk)!=chunk.size()) {finish(false,"保存失败："+state->file->errorString());return;}
        state->next=end; progress->setValue(end-state->first);
        if(state->next>state->last) {
            const bool committed=state->file->commit();
            finish(committed,committed ? QString("已保存 %1 个采样点：\n%2")
                .arg(state->last-state->first+1).arg(QDir::toNativeSeparators(state->path))
                : "保存失败："+state->file->errorString());
        }
    });
    connect(save,&QPushButton::clicked,dialog,[=]() {
        const bool csv=format->currentIndex()==0;
        const bool all=scope->currentIndex()==1;
        const auto range=exportSampleRange(snapshot,all);
        if(csv && range.first<0)return;
        QString folder=m_lastExportDirectory;
        if(folder.isEmpty())folder=QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if(folder.isEmpty())folder=QDir::homePath();
        const QString suffix=csv ? "csv" : "png";
        auto picker=new QFileDialog(dialog,"选择保存位置",folder,csv ? "CSV 数据 (*.csv)" : "PNG 波形图片 (*.png)");
        picker->setObjectName("waveformExportFileDialog"); picker->setAttribute(Qt::WA_DeleteOnClose);
        picker->setOption(QFileDialog::DontUseNativeDialog); picker->setOption(QFileDialog::DontUseCustomDirectoryIcons);
        picker->setAcceptMode(QFileDialog::AcceptSave); picker->setFileMode(QFileDialog::AnyFile); picker->setDefaultSuffix(suffix);
        picker->selectFile("波形_"+snapshot.selectedAt.toString("yyyyMMdd_HHmmss_zzz")+"."+suffix);
        format->setEnabled(false); scope->setEnabled(false); save->setEnabled(false);
        connect(picker,&QDialog::rejected,dialog,[=]() {format->setEnabled(true);refresh();});
        connect(picker,&QDialog::accepted,dialog,[=]() {
            const QString path=picker->selectedFiles().value(0);
            if(path.isEmpty() || QFileInfo(path).suffix().compare(suffix,Qt::CaseInsensitive)!=0) {
                finish(false,"请使用 ."+suffix+" 文件后缀。未保存文件。"); return;
            }
            m_lastExportDirectory=QFileInfo(path).absolutePath();
            state->path=path; state->active=true;
            state->file.reset(new QSaveFile(path));
            if(!state->file->open(QIODevice::WriteOnly)) {finish(false,"无法保存："+state->file->errorString());return;}
            if(!csv) {
                if(snapshot.image.isNull() || !snapshot.image.save(state->file.data(),"PNG")) {finish(false,"图片保存失败，文件未改动。");return;}
                const bool committed=state->file->commit();
                finish(committed,committed ? "图片已保存：\n"+QDir::toNativeSeparators(path) : "保存失败："+state->file->errorString());
                return;
            }
            QString notes=snapshot.notes.join(" | "); notes.replace('\n',' '); notes.replace('\r',' ');
            const QString metadata=QString("# FPGA Signal Analyzer waveform data\n# snapshot_time_local,%1\n# source,%2\n# range,%3\n# sample_rate_hz,%4\n# total_retained_samples,%5\n# exported_sample_count,%6\n# time_origin,retained_data_sample_0 (not trigger time)\n# time_unit,s\n# voltage_unit,V\n# probe_factor_A,%8\n# probe_factor_B,%9\n# voltage_conversion,ADC_voltage * probe_factor (already applied)\n# data_notes,%7\nsample_index,time_s,channel_A_V,channel_B_V\n")
                .arg(snapshot.selectedAt.toString(Qt::ISODateWithMs)).arg(snapshot.source)
                .arg(all ? "全部已接收数据" : "当前画面范围")
                .arg(snapshot.sampleRateHz>0 ? QString::number(snapshot.sampleRateHz,'g',17) : QString())
                .arg(std::max(snapshot.a.size(),snapshot.b.size())).arg(range.second-range.first+1).arg(notes).arg(snapshot.probeA).arg(snapshot.probeB);
            const QByteArray header=QByteArray("\xEF\xBB\xBF",3)+metadata.toUtf8();
            if(state->file->write(header)!=header.size()) {finish(false,"保存失败："+state->file->errorString());return;}
            state->first=state->next=range.first; state->last=range.second;
            status->setStyleSheet("color:#526780;"); status->setText("正在保存 CSV 数据…");
            progress->setRange(0,range.second-range.first+1); progress->setValue(0); progress->show(); cancel->show(); timer->start();
        });
        picker->open();
    });
    refresh(); dialog->show();
}

void WaveformWidget::analyzeWithAi()
{
    if(m_aiDialog){m_aiDialog->show();m_aiDialog->raise();m_aiDialog->activateWindow();return;}
    const bool latest = m_cursorMode == NoCursor && !m_displayPaused;
    const QVector<double> dataA = analysisData(0, latest);
    const QVector<double> dataB = analysisData(1, latest);
    // Keep both channels from the same instant. Switching channels never samples a newer frame.
    const double sampleRateHz = latest ? m_latestSampleRateHz : m_sampleRateHz;
    const QStringList acquisitionWarnings = latest ? m_latestAcquisitionWarnings : m_visibleAcquisitionWarnings;
    const bool recordSnapshot = m_recordMode;
    const double probeA = m_probeScale[0], probeB = m_probeScale[1];
    const auto prepareSnapshot = [=](int channel) -> AiWaveformSnapshot {
        AiWaveformSnapshot snapshot;
        snapshot.data = channel == 1 ? dataB : dataA;
        snapshot.channelName = analysisChannelName(channel);
        snapshot.probeScale = channel == 0 ? probeA : probeB;
        snapshot.stats = calculateWaveformStatistics(snapshot.data, 0, snapshot.data.size() - 1);
        snapshot.frequency = estimateFrequency(snapshot.data, sampleRateHz, snapshot.probeScale);
        snapshot.spectrum = analyzeSpectrum(snapshot.data, sampleRateHz);
        snapshot.quality = waveformQualityWarnings(snapshot.data, sampleRateHz, acquisitionWarnings, snapshot.probeScale);
        if (snapshot.data.isEmpty()) snapshot.quality.prepend(snapshot.channelName + "暂无采样数据。");
        snapshot.evidence = WaveformAi::buildEvidence(snapshot.data, sampleRateHz, snapshot.probeScale,
            snapshot.channelName, snapshot.quality, recordSnapshot, snapshot.stats, snapshot.frequency, snapshot.spectrum);
        return snapshot;
    };
    if ((m_aiChannelIndex == 0 ? dataA : dataB).isEmpty() && (!dataA.isEmpty() || !dataB.isEmpty()))
        m_aiChannelIndex = dataA.isEmpty() ? 1 : 0;

    QDialog *dialog = new QDialog(this);
    m_aiDialog=dialog;
    dialog->setObjectName("aiAnalysisDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("AI 分析当前波形 · " + analysisChannelName(m_aiChannelIndex));
    dialog->setProperty("probeFactorA",probeA); dialog->setProperty("probeFactorB",probeB);
    dialog->resize(900, 1000);
    dialog->setMinimumSize(720, 780);
    dialog->setStyleSheet(
        "QDialog#aiAnalysisDialog { background: #f3f6fa; }"
        "QDialog#aiAnalysisDialog QWidget { font-family: 'Microsoft YaHei'; font-size: 13px; color: #243247; }"
        "QDialog#aiAnalysisDialog QFrame[card='true'] { background: white; border: 1px solid #dce4ee; border-radius: 10px; }"
        "QDialog#aiAnalysisDialog QLabel { background: transparent; border: none; }"
        "QDialog#aiAnalysisDialog QLabel[muted='true'] { color: #69788d; font-size: 12px; }"
        "QDialog#aiAnalysisDialog QLabel[heading='true'] { font-size: 14px; font-weight: bold; }"
        "QDialog#aiAnalysisDialog QLineEdit, QDialog#aiAnalysisDialog QComboBox { background: white; border: 1px solid #cfd9e6; border-radius: 5px; padding: 7px 10px; min-height: 18px; }"
        "QDialog#aiAnalysisDialog QLineEdit:focus, QDialog#aiAnalysisDialog QComboBox:focus { border-color: #3679df; }"
        "QDialog#aiAnalysisDialog QPushButton { background: white; border: 1px solid #cfd9e6; border-radius: 6px; padding: 8px 18px; }"
        "QDialog#aiAnalysisDialog QPushButton:hover { background: #edf3fc; border-color: #8aaddc; }"
        "QDialog#aiAnalysisDialog QPushButton#aiStartButton { background: #2869d8; color: white; border-color: #2869d8; font-weight: bold; }"
        "QDialog#aiAnalysisDialog QPushButton#aiStartButton:hover { background: #1e58bb; }"
        "QDialog#aiAnalysisDialog QPushButton:disabled { background: #e8edf4; color: #98a4b4; border-color: #dce4ee; }"
        "QDialog#aiAnalysisDialog QTextEdit { background: white; border: none; padding: 4px; selection-background-color: #d8e7ff; }"
        "QDialog#aiAnalysisDialog QProgressBar { border: none; background: #e8eff9; max-height: 3px; }"
        "QDialog#aiAnalysisDialog QProgressBar::chunk { background: #3679df; }");
    QVBoxLayout *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(10);
    QHBoxLayout *header = new QHBoxLayout();
    QVBoxLayout *titles = new QVBoxLayout();
    titles->setSpacing(4);
    QLabel *title = new QLabel("AI 分析", dialog);
    title->setStyleSheet("font-size: 23px; font-weight: bold; color: #182b46;");
    titles->addWidget(title);
    QLabel *subtitle = new QLabel("解释测量结果 · 辅助排查异常\n快照探头倍率：A "+probeDescription(0)+"，B "+probeDescription(1), dialog);
    subtitle->setObjectName("label_aiProbeFactors");
    subtitle->setProperty("muted", true);
    titles->addWidget(subtitle);
    header->addLayout(titles);
    header->addStretch();
    QComboBox *channelEdit = new QComboBox(dialog);
    channelEdit->setObjectName("aiChannelSelector");
    channelEdit->addItems({"A 通道", "B 通道"});
    channelEdit->setCurrentIndex(m_aiChannelIndex);
    auto channelTitle=new QLabel("分析通道：",dialog);header->addWidget(channelTitle);
    header->addWidget(channelEdit);
    QLabel *status = new QLabel("待分析", dialog);
    status->setObjectName("aiStatus");
    auto setStatus = [this, status, channelEdit](const QString &text, const QString &color, const QString &background) {
        status->setText(text);
        status->setStyleSheet(QString("color: %1; background: %2; border-radius: 12px; padding: 5px 12px;").arg(color, background));
        m_aiStatusLabel->setText("AI 分析（" + channelEdit->currentText() + "）：" + text + "，详情见 AI 分析窗口。");
        m_aiStatusLabel->setStyleSheet("color: " + color + ";");
    };
    setStatus("待分析", "#526780", "#e6edf5");
    header->addWidget(status, 0, Qt::AlignVCenter);
    layout->addLayout(header);
    QFrame *settings = new QFrame(dialog);
    settings->setProperty("card", true);
    QVBoxLayout *settingsLayout = new QVBoxLayout(settings);
    settingsLayout->setContentsMargins(16, 12, 16, 12);
    settingsLayout->setSpacing(10);
    QLabel *settingsTitle = new QLabel("连接设置", settings);
    settingsTitle->setProperty("heading", true);
    settingsLayout->addWidget(settingsTitle);
    QFormLayout *form = new QFormLayout();
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(8);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    QComboBox *providerEdit = new QComboBox(dialog);
    providerEdit->setObjectName("aiProviderSelector");
    providerEdit->addItem("DeepSeek", AiApi::DeepSeek);
    providerEdit->addItem("Gemini（有免费套餐）", AiApi::Gemini);
    providerEdit->addItem("OpenAI（按量计费）", AiApi::OpenAI);
    QLineEdit *keyEdit = new QLineEdit(dialog);
    keyEdit->setObjectName("aiApiKey");
    keyEdit->setEchoMode(QLineEdit::Password);
    keyEdit->setPlaceholderText("输入 DeepSeek API Key；关闭后不保存");
    const QByteArray envKey = qgetenv("DEEPSEEK_API_KEY");
    if (!envKey.isEmpty()) keyEdit->setText(QString::fromUtf8(envKey));
    QLineEdit *modelEdit = new QLineEdit("deepseek-flash", dialog);
    modelEdit->setObjectName("aiApiModel");
    form->addRow("服务商", providerEdit);
    form->addRow("API Key", keyEdit);
    form->addRow("模型", modelEdit);
    QComboBox *analysisMode = new QComboBox(dialog);
    analysisMode->setObjectName("aiAnalysisMode");
    analysisMode->addItem("波形分析", 0);
    analysisMode->addItem("幅频 / 相频分析（单路 FFT）", 2);
    analysisMode->addItem("电路测试分析",10);
    analysisMode->addItem("理论与实测对照",11);
    analysisMode->addItem("异常排查",12);
    analysisMode->addItem("PLL / 幅值闭环过程分析",13);
    form->addRow("分析内容", analysisMode);
    QLabel *keyHelp = new QLabel("<a href=\"https://platform.deepseek.com/api_keys\">在 DeepSeek 开放平台创建 API Key</a>", dialog);
    keyHelp->setOpenExternalLinks(true);
    form->addRow(keyHelp);
    connect(providerEdit, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
            [=](int index) {
        const AiApi::Provider selected = static_cast<AiApi::Provider>(providerEdit->itemData(index).toInt());
        if (selected == AiApi::DeepSeek) {
            modelEdit->setText("deepseek-flash");
            keyEdit->setText(QString::fromUtf8(qgetenv("DEEPSEEK_API_KEY")));
            keyEdit->setPlaceholderText("输入 DeepSeek API Key；关闭后不保存");
            keyHelp->setText("<a href=\"https://platform.deepseek.com/api_keys\">在 DeepSeek 开放平台创建 API Key</a>");
            return;
        }
        const bool gemini = selected == AiApi::Gemini;
        modelEdit->setText(gemini ? "gemini-2.5-flash-lite" : "gpt-4.1-mini");
        keyEdit->setText(QString::fromUtf8(qgetenv(gemini ? "GEMINI_API_KEY" : "OPENAI_API_KEY")));
        keyEdit->setPlaceholderText(gemini ? "输入 Gemini API Key；关闭后不保存"
                                           : "输入 OpenAI API Key；关闭后不保存");
        keyHelp->setText(gemini
            ? "<a href=\"https://aistudio.google.com/apikey\">在 Google AI Studio 创建 Gemini Key</a>"
            : "<a href=\"https://platform.openai.com/api-keys\">在 OpenAI 平台创建 Key</a>");
    });
    settingsLayout->addLayout(form);
    layout->addWidget(settings);
    auto rootLayout=layout;
    auto pages=new QStackedWidget(dialog);pages->setObjectName("stack_aiContent");rootLayout->addWidget(pages,1);
    auto waveformPage=new QWidget(pages);pages->addWidget(waveformPage);
    layout=new QVBoxLayout(waveformPage);layout->setContentsMargins(0,0,0,0);layout->setSpacing(10);
    const auto source=m_experimentSource;
    auto experiment=new ExperimentAiDialog([source](){
        if(source)return source();ExperimentAnalysis::Snapshot empty;empty.context="尚无电路或控制实验数据";return empty;
    },0,pages,nullptr,true);pages->addWidget(experiment);
    const auto syncService=[=](){experiment->setService(providerEdit->currentData().toInt(),modelEdit->text(),keyEdit->text());};
    connect(providerEdit,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[=](int){syncService();});
    connect(modelEdit,&QLineEdit::textChanged,dialog,[=](const QString &){syncService();});
    connect(keyEdit,&QLineEdit::textChanged,dialog,[=](const QString &){syncService();});syncService();
    experiment->requestBusy=[=](bool busy){providerEdit->setEnabled(!busy);keyEdit->setEnabled(!busy);modelEdit->setEnabled(!busy);analysisMode->setEnabled(!busy);};
    QFrame *summary = new QFrame(dialog);
    summary->setProperty("card", true);
    QGridLayout *summaryLayout = new QGridLayout(summary);
    summaryLayout->setContentsMargins(16, 12, 16, 12);
    summaryLayout->setHorizontalSpacing(20);
    const QStringList metricNames = {"通道快照", "有效采样率", "峰峰值", "RMS"};
    QVector<QLabel *> metricLabels, metricValues;
    for (int i = 0; i < metricNames.size(); ++i) {
        QLabel *name = new QLabel(metricNames[i], summary);
        name->setProperty("muted", true);
        QLabel *value = new QLabel(summary);
        value->setObjectName(QString("aiMetric%1").arg(i));
        metricLabels.append(name);
        metricValues.append(value);
        value->setStyleSheet("font-size: 17px; font-weight: bold; color: #234369;");
        summaryLayout->addWidget(name, 0, i);
        summaryLayout->addWidget(value, 1, i);
        summaryLayout->setColumnStretch(i, 1);
    }
    layout->addWidget(summary);
    QFrame *qualityCard = new QFrame(dialog);
    qualityCard->setProperty("card", true);
    QVBoxLayout *qualityLayout = new QVBoxLayout(qualityCard);
    qualityLayout->setContentsMargins(16, 10, 16, 10);
    QLabel *qualityTitle = new QLabel("数据质量 · 本地检查", qualityCard);
    qualityTitle->setProperty("heading", true);
    qualityLayout->addWidget(qualityTitle);
    QTextEdit *qualityText = new QTextEdit(qualityCard);
    qualityText->setObjectName("aiQualityWarnings");
    qualityText->setReadOnly(true);
    qualityText->setMinimumHeight(50);
    qualityText->setMaximumHeight(70);
    qualityText->setStyleSheet("color: #966111; background: #fff8eb; border: none; border-radius: 5px; padding: 4px;");
    qualityLayout->addWidget(qualityText);
    layout->addWidget(qualityCard);
    QFrame *report = new QFrame(dialog);
    report->setProperty("card", true);
    QVBoxLayout *reportLayout = new QVBoxLayout(report);
    reportLayout->setContentsMargins(16, 12, 16, 12);
    QHBoxLayout *reportHeader = new QHBoxLayout();
    QLabel *reportTitle = new QLabel("分析结果", report);
    reportTitle->setProperty("heading", true);
    reportHeader->addWidget(reportTitle);
    reportHeader->addStretch();
    QPushButton *copy = new QPushButton("复制结果", report);
    copy->setEnabled(false);
    reportHeader->addWidget(copy);
    reportLayout->addLayout(reportHeader);
    QProgressBar *progress = new QProgressBar(report);
    progress->setRange(0, 0);
    progress->setTextVisible(false);
    progress->hide();
    reportLayout->addWidget(progress);
    QTextEdit *result = new QTextEdit(report);
    result->setObjectName("aiResult");
    result->setReadOnly(true);
    result->document()->setDefaultStyleSheet("p { margin-top: 6px; margin-bottom: 10px; } li { margin-bottom: 6px; } h2, h3 { color: #234369; }");
    result->setHtml("<p style='color:#69788d'>已准备好当前波形快照。</p><p>填写 API Key 后，点击下方<b>开始分析</b>查看波形特征和排查建议。</p>");
    QTabWidget *reportTabs = new QTabWidget(report);
    reportTabs->setObjectName("tabs_aiWaveReport");
    reportTabs->setStyleSheet("QTabWidget::pane { border: none; } QTabBar::tab { background: #edf2f8; padding: 7px 16px; margin-right: 4px; } QTabBar::tab:selected { background: #dce9fc; color: #2869d8; }");
    reportTabs->addTab(result, "分析报告");
    QChart *snapshotChart = new QChart();
    snapshotChart->legend()->hide();
    snapshotChart->setAnimationOptions(QChart::NoAnimation);
    QLineSeries *snapshotLine = new QLineSeries();
    snapshotLine->setPen(QPen(QColor("#2869d8"), 1.2));
    snapshotChart->addSeries(snapshotLine);
    QValueAxis *snapshotX = new QValueAxis();
    snapshotX->setTitleText("频率 (kHz)");
    snapshotX->setRange(0, std::max(1.0, sampleRateHz / 2000.0));
    snapshotX->setTickCount(6);
    QValueAxis *snapshotY = new QValueAxis();
    snapshotY->setTitleText("幅度 (dBV RMS)");
    snapshotY->setRange(-120, 10);
    snapshotChart->addAxis(snapshotX, Qt::AlignBottom);
    snapshotChart->addAxis(snapshotY, Qt::AlignLeft);
    snapshotLine->attachAxis(snapshotX);
    snapshotLine->attachAxis(snapshotY);
    QChartView *snapshotView = new QChartView(snapshotChart, reportTabs);
    snapshotView->setMinimumHeight(260);
    snapshotView->setObjectName("aiSpectrumSnapshot");
    reportTabs->addTab(snapshotView, "幅频图 / 频谱");
    QChart *phaseChart = new QChart();
    phaseChart->legend()->hide();
    phaseChart->setAnimationOptions(QChart::NoAnimation);
    QScatterSeries *phasePoints = new QScatterSeries();
    phasePoints->setColor(QColor("#8055cf"));
    phasePoints->setMarkerSize(5.0);
    phaseChart->addSeries(phasePoints);
    QValueAxis *phaseX = new QValueAxis();
    phaseX->setTitleText("频率 (kHz)");
    phaseX->setRange(0, std::max(1.0, sampleRateHz / 2000.0));
    phaseX->setTickCount(6);
    QValueAxis *phaseY = new QValueAxis();
    phaseY->setTitleText("相位 (°)");
    phaseY->setRange(-180, 180);
    phaseY->setTickCount(5);
    phaseChart->addAxis(phaseX, Qt::AlignBottom);
    phaseChart->addAxis(phaseY, Qt::AlignLeft);
    phasePoints->attachAxis(phaseX);
    phasePoints->attachAxis(phaseY);
    QChartView *phaseView = new QChartView(phaseChart, reportTabs);
    phaseView->setMinimumHeight(260);
    phaseView->setObjectName("aiPhaseSnapshot");
    phaseView->setToolTip("相位单位为度，范围 -180° 至 +180°；以本次 FFT 数据段起点为时间零点，使用余弦参考。低于显示门限的分量不显示相位。");
    reportTabs->addTab(phaseView, "相频图（单路 FFT）");
    reportLayout->addWidget(reportTabs, 1);
    layout->addWidget(report, 1);
    QLabel *note = new QLabel("测量值由程序计算；AI 根据本地结果选择解释与检查建议。使用打开窗口时的快照，点击分析后才发送摘要和最多 512 个带原始索引的抽取值。API 按平台账户计费。", dialog);
    note->setWordWrap(true);
    note->setProperty("muted", true);
    layout->addWidget(note);
    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->button(QDialogButtonBox::Close)->setText("关闭");
    QPushButton *start = buttons->addButton("开始分析", QDialogButtonBox::ActionRole);
    start->setObjectName("aiStartButton");
    start->setStyleSheet("QPushButton { background: #2869d8; color: white; border: 1px solid #2869d8; border-radius: 6px; padding: 8px 18px; font-weight: bold; } QPushButton:hover { background: #1e58bb; } QPushButton:disabled { background: #e8edf4; color: #98a4b4; border-color: #dce4ee; }");
    start->setDefault(true);
    layout->addWidget(buttons);
    const auto refreshSnapshot = [=](int channel) {
        m_aiChannelIndex = channel;
        const AiWaveformSnapshot current = prepareSnapshot(channel);
        const SpectrumResult &spectrum = current.spectrum;
        const WaveformStatistics &stats = current.stats;
        dialog->setWindowTitle("AI 分析当前波形 · " + current.channelName);
        dialog->setProperty("snapshotChannel", current.channelName);
        metricLabels[0]->setText(current.channelName + "快照");
        metricValues[0]->setText(QString("%1 点").arg(current.data.size()));
        metricValues[1]->setText(sampleRateHz > 0 ? QString("%1 kSa/s").arg(sampleRateHz/1000.0,0,'f',3) : "未知");
        metricValues[2]->setText(stats.valid ? QString("%1 V").arg(stats.peakToPeak,0,'f',3) : "—");
        metricValues[3]->setText(stats.valid ? QString("%1 V").arg(stats.rms,0,'f',3) : "—");
        qualityText->setPlainText("• " + current.quality.join("\n• "));
        snapshotLine->clear();
        phasePoints->clear();
        if (spectrum.valid) {
            snapshotChart->setTitle(QString("%1快照 · Hann 窗 · %2 点 · Δf = %3 Hz")
                .arg(current.channelName).arg(spectrum.fftSize).arg(spectrum.resolutionHz,0,'f',2));
            snapshotY->setRange(-120,std::max(10.0,spectrum.peakDbv+5));
            QVector<QPointF> bins, phases;
            const double threshold = std::max(-100.0,spectrum.peakDbv-40.0);
            for (int i=0;i<spectrum.dbv.size();++i) {
                bins.append(QPointF(i*spectrum.resolutionHz/1000.0,spectrum.dbv[i]));
                if (i>0 && i<spectrum.dbv.size()-1 && spectrum.dbv[i]>=threshold)
                    phases.append(QPointF(i*spectrum.resolutionHz/1000.0,spectrum.phaseDegrees[i]));
            }
            snapshotLine->replace(bins);
            phasePoints->replace(phases);
            phaseChart->setTitle(QString("%1相位快照 · FFT 段起点为参考 · 仅显示 ≥ %2 dBV 分量")
                .arg(current.channelName).arg(threshold,0,'f',1));
        } else {
            snapshotChart->setTitle(current.channelName + "暂无有效频谱：需要有效采样率、至少 256 点及可识别的信号。");
            phaseChart->setTitle(current.channelName + "暂无有效相位谱。");
        }
        start->setText("开始分析");
        start->setEnabled(!current.data.isEmpty());
        copy->setEnabled(!current.data.isEmpty());
        reportTabs->setCurrentIndex(0);
        if (current.data.isEmpty()) result->setPlainText(current.channelName + "暂无采样数据，请选择另一个通道。");
        else result->setMarkdown(current.evidence.localReport + "填写 API Key 后点击“开始分析”，获取有测量依据的解释和检查建议。");
        setStatus(current.data.isEmpty() ? "暂无数据" : "待分析", "#526780", "#e6edf5");
    };
    connect(channelEdit,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,refreshSnapshot);
    refreshSnapshot(channelEdit->currentIndex());
    connect(analysisMode,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[=](int){
        const int mode=analysisMode->currentData().toInt();const bool experimental=mode>=10;
        pages->setCurrentIndex(experimental?1:0);channelEdit->setVisible(!experimental);channelTitle->setVisible(!experimental);
        subtitle->setVisible(!experimental);
        if(experimental){experiment->setAnalysisMode(mode-10);dialog->setWindowTitle("AI 分析 · "+analysisMode->currentText());
            status->setText("实验分析");m_aiStatusLabel->setText("AI 分析："+analysisMode->currentText()+"，详情见分析窗口。");}
        else refreshSnapshot(channelEdit->currentIndex());
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(copy, &QPushButton::clicked, dialog, [result]() {
        QApplication::clipboard()->setText(result->toPlainText());
    });
    QNetworkAccessManager *network = m_aiNetwork ? m_aiNetwork.data() : new QNetworkAccessManager(dialog);
    connect(start, &QPushButton::clicked, dialog,
            [=]() {
        if(analysisMode->currentData().toInt()>=10)return;
        const AiWaveformSnapshot current = prepareSnapshot(channelEdit->currentIndex());
        if (current.data.isEmpty()) return;
        const QVector<double> &data = current.data;
        const QString key = keyEdit->text().trimmed();
        const QString model = modelEdit->text().trimmed();
        const AiApi::Provider provider = static_cast<AiApi::Provider>(providerEdit->currentData().toInt());
        if (key.isEmpty() || model.isEmpty()) {
            setStatus("请完善设置", "#9a6513", "#fff2d9");
            result->setMarkdown(current.evidence.localReport + "**请填写 API Key 和模型名称，再请求 AI 分析。**");
            return;
        }
        if (!QSslSocket::supportsSsl()) {
            setStatus("连接不可用", "#b84040", "#fde9e9");
            result->setMarkdown(current.evidence.localReport + "**当前电脑缺少 Qt 所需的 OpenSSL 运行库，无法建立 HTTPS 连接。请使用随包附带的完整程序目录。**");
            return;
        }
        start->setEnabled(false);
        channelEdit->setEnabled(false);
        keyEdit->setEnabled(false);
        modelEdit->setEnabled(false);
        providerEdit->setEnabled(false);
        analysisMode->setEnabled(false);
        copy->setEnabled(false);
        progress->show();
        start->setText("正在分析…");
        setStatus("分析中", "#2869d8", "#e7efff");
        result->setMarkdown(current.evidence.localReport + "正在请求 AI 解释与检查建议…");
        const QString description = WaveformAi::requestInput(current.evidence, data,
            analysisMode->currentData().toInt() == 2);
        const QJsonObject body = AiApi::requestBody(provider, model,
            WaveformAi::instructions(), description, 1200);
        QNetworkRequest request(AiApi::endpoint(provider));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
        QNetworkReply *reply = network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        QTimer *timer = new QTimer(reply);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
        timer->start(60000);
        connect(reply, &QNetworkReply::finished, dialog, [=]() {
            timer->stop();
            start->setEnabled(true);
            channelEdit->setEnabled(true);
            keyEdit->setEnabled(true);
            modelEdit->setEnabled(true);
            providerEdit->setEnabled(true);
            analysisMode->setEnabled(true);
            progress->hide();
            start->setText("重新分析");
            const QByteArray payload = reply->readAll();
            if (reply->error() != QNetworkReply::NoError) {
                setStatus("分析失败", "#b84040", "#fde9e9");
                const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                result->setMarkdown(current.evidence.localReport);
                result->append("AI 请求失败：" + AiApi::errorMessage(payload, reply->errorString(), key, httpStatus).toHtmlEscaped());
                copy->setEnabled(true);
            } else {
                const QJsonObject root = QJsonDocument::fromJson(payload).object();
                const QString text = AiApi::responseText(provider, root);
                QString checkedReport;
                if (WaveformAi::incompleteResponse(root)) {
                    setStatus("回复未完整", "#9a6513", "#fff2d9");
                    result->setMarkdown(current.evidence.localReport + "**AI 回复未完整返回，本次不显示其分析。可重新分析；以上本地测量结果保留。**");
                } else if (!WaveformAi::renderSelection(text, current.evidence, checkedReport)) {
                    setStatus("回复未通过核对", "#9a6513", "#fff2d9");
                    result->setMarkdown(current.evidence.localReport + "**AI 回复格式或依据未通过核对，本次不显示其分析。可重新分析或更换模型；以上本地测量结果保留。**");
                } else {
                    result->setMarkdown(checkedReport);
                    setStatus("分析完成", "#20805b", "#e3f4ec");
                }
                copy->setEnabled(true);
            }
            reply->deleteLater();
        });
        connect(dialog, &QObject::destroyed, reply, [reply]() { if (!reply->isFinished()) reply->abort(); });
    });
    dialog->show();
}

void WaveformWidget::setWaveformData(const QVector<double> &data, int maxPoints)
{
    setWaveformData(data, QVector<double>(), maxPoints);
}

const QVector<double> &WaveformWidget::rawAnalysisData(int channel, bool latest) const
{
    if (m_displayPaused) latest = false;
    if (channel == 1) return latest ? m_lastDataB : m_visibleDataB;
    return latest ? m_lastData : m_visibleData;
}

QVector<double> WaveformWidget::analysisData(int channel, bool latest) const
{
    return probeScaled(rawAnalysisData(channel,latest),m_probeScale[channel == 1 ? 1 : 0]);
}

QString WaveformWidget::probeDescription(int channel) const
{
    const double factor=m_probeScale[channel == 1 ? 1 : 0];
    return factor==0.5 ? "×1/2" : factor==2.0 ? "×2" : "×1";
}

void WaveformWidget::changeProbeScale(int channel)
{
    const double factor=m_probeSelectors[channel]->currentData().toDouble();
    if ((factor!=0.5 && factor!=1.0 && factor!=2.0) || factor==m_probeScale[channel])return;
    m_probeScale[channel]=factor;
    // Keep raw ADC volts and the held source intact; never rescale received buffers.
    if(m_recordMode)renderRecord();else renderPreview();
    m_hasVoltageCursor[0]=m_hasVoltageCursor[1]=false; // Shared voltage guides no longer have an unambiguous channel scale.
    fitRecordVoltage();updateCursorLines();updateMeasurements();updateStatistics();
    m_axisY->setTitleText("电压 (V，按探头倍率)");
}

QString WaveformWidget::analysisChannelName(int channel) const
{
    return channel == 1 ? "B 通道" : "A 通道";
}

void WaveformWidget::setWaveformData(const QVector<double> &data, const QVector<double> &dataB, int maxPoints)
{
    const bool hadData = !m_visibleData.isEmpty();
    m_recordMode = false;
    m_frequencyButton->setText(m_frequencyButton->isChecked() ? "实时测频" : "继续测频");
    if (!m_displayPaused || data.isEmpty())
        m_chart->setTitle("实时波形（滚轮：横轴；Shift+滚轮：纵轴；右键：复位）");
    if (data.isEmpty()) {
        m_displayPaused = false;
        m_latestAcquisitionWarnings.clear();
        m_visibleAcquisitionWarnings.clear();
        m_lastData.clear();
        m_visibleData.clear();
        m_lastDataB.clear();
        m_visibleDataB.clear();
        m_series->clear();
        m_seriesB->clear();
        m_sampleRateHz = 0;
        m_latestSampleRateHz = 0;
        m_frequencyDataAge.invalidate();
        m_frequencyResults.clear();
        m_frequencyDirty = false;
        clearCursors();
        setSampleRange(0, 999);
        m_autoFitY = false;
        updateFrequencyDisplay();
        updateStatistics();
        emit displayStateChanged();
        return;
    }
    m_lastData = data;
    m_lastDataB = dataB.size() == data.size() ? dataB : QVector<double>();
    m_frequencyDataAge.start();
    m_frequencyDirty = true;
    // 测量时冻结屏幕，光标对应同一份采样数据；TCP 接收继续进行。
    if (m_displayPaused || m_cursorMode != NoCursor) return;
    m_visibleAcquisitionWarnings = m_latestAcquisitionWarnings;
    m_visibleData = data;
    m_visibleDataB = m_lastDataB;
    m_previewMaxPoints = std::max(1,maxPoints);

    if (m_autoScroll) {
        int totalPoints = data.size();
        int windowSize = 1000;
        if (totalPoints > windowSize) {
            setSampleRange(totalPoints - windowSize, totalPoints - 1);
        } else {
            setSampleRange(0, windowSize - 1);
        }
    } else setSampleRange(axisToSample(m_axisX->min()), axisToSample(m_axisX->max()));
    if (m_autoFitY) {
        double minValue = std::numeric_limits<double>::infinity();
        double maxValue = -std::numeric_limits<double>::infinity();
        for (int channel = 0; channel < 2; ++channel) {
            const QVector<double> &values = channel == 0 ? m_visibleData : m_visibleDataB;
            if (!(channel == 0 ? m_showA->isChecked() : m_showB->isChecked())) continue;
            for (int i = std::max(0, values.size() - 1000); i < values.size(); ++i) {
                if (!std::isfinite(values[i])) continue;
                minValue = std::min(minValue, values[i]*m_probeScale[channel]);
                maxValue = std::max(maxValue, values[i]*m_probeScale[channel]);
            }
        }
        if (std::isfinite(minValue) && std::isfinite(maxValue)) {
            const double span = maxValue - minValue;
            const double margin = std::max(0.05, span * 0.10);
            m_axisY->setRange(minValue - margin, maxValue + margin);
        }
    }
    updateStatistics();
    if (!hadData) emit displayStateChanged();
}

double WaveformWidget::sampleToAxis(double sample) const
{
    return sample * m_xUnitsPerSample;
}

double WaveformWidget::axisToSample(double coordinate) const
{
    const double sample = coordinate / m_xUnitsPerSample;
    const double nearest = std::round(sample);
    // Avoid losing an endpoint when a time coordinate represents an exact sample.
    return std::abs(sample - nearest) < 1e-7 ? nearest : sample;
}

void WaveformWidget::setSampleRange(double first, double last)
{
    if (!std::isfinite(first) || !std::isfinite(last) || last <= first || m_updatingTimeAxis) return;
    m_updatingTimeAxis = true;
    QString unit;
    if (m_sampleRateHz > 0) {
        const double seconds = (last - first) / m_sampleRateHz;
        const double secondsPerUnit = seconds >= 1.0 ? 1.0 : seconds >= 0.001 ? 0.001 : 0.000001;
        unit = secondsPerUnit == 1.0 ? "s" : secondsPerUnit == 0.001 ? "ms" : "µs";
        m_xUnitsPerSample = 1.0 / (m_sampleRateHz * secondsPerUnit);
        m_axisX->setTitleText("时间（" + unit + "）");
        m_chartView->setToolTip(m_recordMode
            ? "横轴从完整记录的首个采样点计时；不是触发时刻。"
            : "横轴从当前保留的预览数据首点计时；不是板端运行时间。缩放只改变显示范围，不改变采样率。");
    } else {
        m_xUnitsPerSample = 1.0;
        m_axisX->setTitleText("采样点（采样率未知）");
        m_chartView->setToolTip("有效采样率未知，暂按采样点显示；收到有效采样率后显示时间。");
    }
    const double tickStep = (last - first) * m_xUnitsPerSample / (m_axisX->tickCount() - 1);
    const int decimals = qBound(0, int(2 - std::floor(std::log10(std::max(tickStep, 1e-12)))), 9);
    m_axisX->setLabelFormat(QString("%.") + QString::number(decimals) + "f");
    m_axisX->setRange(sampleToAxis(first), sampleToAxis(last));
    m_updatingTimeAxis = false;
    if (m_recordMode) renderRecord(); else renderPreview();
    updateCursorLines();
    updateStatistics();
}

void WaveformWidget::renderPreview()
{
    const auto &data=m_visibleData;
    QVector<QPointF> points;
    const int step = std::max(1, int((qint64(data.size()) + m_previewMaxPoints - 1) / m_previewMaxPoints));
    points.reserve(data.size() / step + 1);

    for (int i = 0; i < data.size(); i += step) {
        points.append(QPointF(sampleToAxis(i), data[i]*m_probeScale[0]));
    }
    m_series->replace(points);
    QVector<QPointF> pointsB;
    pointsB.reserve(points.size());
    for (int i = 0; i < m_visibleDataB.size(); i += step)
        pointsB.append(QPointF(sampleToAxis(i), m_visibleDataB[i]*m_probeScale[1]));
    m_seriesB->replace(pointsB);
}

void WaveformWidget::setDisplayPaused(bool paused)
{
    if (paused == m_displayPaused || (paused && !canPauseDisplay())) return;
    m_displayPaused = paused;
    if (paused) {
        m_autoScroll = false;
        m_autoFitY = false;
        m_chart->setTitle("波形已暂停（可缩放、拖动和测量；点击“继续波形”查看最新数据）");
    } else {
        // Continuing always returns to the latest samples, including after cursor use.
        clearCursors();
    }
    if (m_frequencyButton->isChecked()) autoMeasureFrequency();
    else updateFrequencyDisplay();
    emit displayStateChanged();
}

void WaveformWidget::setRecordedWaveformData(const QVector<double> &data,
                                            const QVector<double> &dataB, double rate)
{
    setWaveformData(QVector<double>()); // Release cursors and the previous source.
    if (data.isEmpty()) return;
    m_recordMode = true;
    m_frequencyButton->setText(m_frequencyButton->isChecked() ? "记录测频" : "继续测频");
    m_autoScroll = false;
    m_autoFitY = false;
    m_lastData = data;
    m_lastDataB = dataB.size() == data.size() ? dataB : QVector<double>();
    m_visibleData = m_lastData;
    m_visibleDataB = m_lastDataB;
    m_latestSampleRateHz = m_sampleRateHz = std::isfinite(rate) && rate > 0 ? rate : 0;
    m_frequencyDirty = true;
    m_chart->setTitle("采集记录（已冻结；滚轮缩放；拖动查看；右键查看全记录）");
    setSampleRange(0, std::max(1, data.size() - 1));
    renderRecord();
    fitRecordVoltage();
    updateStatistics();
    autoMeasureFrequency();
    emit displayStateChanged();
}

void WaveformWidget::renderRecord()
{
    if (!m_recordMode || m_visibleData.isEmpty()) return;
    const double first = std::max(0.0, axisToSample(m_axisX->min()));
    const double last = std::min(double(m_visibleData.size() - 1), axisToSample(m_axisX->max()));
    if (first > last) {
        m_series->clear(); m_seriesB->clear(); return;
    }
    const int begin = int(std::floor(first)), end = int(std::ceil(last));
    for(int channel=0;channel<2;++channel){
        auto points=recordEnvelope(channel==0?m_visibleData:m_visibleDataB,begin,end);
        for(auto &p:points) { p.setX(sampleToAxis(p.x())); p.setY(p.y()*m_probeScale[channel]); }
        (channel==0?m_series:m_seriesB)->replace(points);
    }
}

void WaveformWidget::fitRecordVoltage()
{
    double low = std::numeric_limits<double>::infinity(), high = -low;
    for (int channel = 0; channel < 2; ++channel) {
        if (!(channel == 0 ? m_showA->isChecked() : m_showB->isChecked())) continue;
        const auto &values = channel == 0 ? m_visibleData : m_visibleDataB;
        for (double value : values) {
            if (!std::isfinite(value)) continue;
            low = std::min(low, value*m_probeScale[channel]); high = std::max(high, value*m_probeScale[channel]);
        }
    }
    if (std::isfinite(low) && std::isfinite(high)) {
        const double margin = std::max(0.05, (high - low) * 0.10);
        m_axisY->setRange(low - margin, high + margin);
    }
}

void WaveformWidget::updateStatistics()
{
    QStringList lines;
    lines << "波形统计（当前窗口，A "+probeDescription(0)+" / B "+probeDescription(1)+"）：";
    for (int channel = 0; channel < 2; ++channel) {
        const auto &values = rawAnalysisData(channel, false);
        const QString name = analysisChannelName(channel);
        if (values.isEmpty()) { lines << name + "：暂无数据。"; continue; }
        const double first = std::max(0.0, axisToSample(m_axisX->min()));
        const double last = std::min(double(values.size() - 1), axisToSample(m_axisX->max()));
        WaveformStatistics stats = calculateWaveformStatistics(
            values, first <= last ? int(std::ceil(first)) : 0,
            first <= last ? int(std::floor(last)) : -1);
        if (!stats.valid) { lines << name + "：当前窗口无采样点。"; continue; }
        stats.mean*=m_probeScale[channel];stats.peakToPeak*=m_probeScale[channel];
        stats.minimum*=m_probeScale[channel];stats.maximum*=m_probeScale[channel];stats.rms*=m_probeScale[channel];
        lines << QString("%1（%2 点）：平均值 %3 V；峰峰值 %4 V；最大值 %5 V；最小值 %6 V；RMS %7 V")
            .arg(name).arg(stats.count).arg(stats.mean, 0, 'f', 4)
            .arg(stats.peakToPeak, 0, 'f', 4).arg(stats.maximum, 0, 'f', 4)
            .arg(stats.minimum, 0, 'f', 4).arg(stats.rms, 0, 'f', 4);
    }
    m_statisticsLabel->setText(lines.join("\n"));
}

void WaveformWidget::autoMeasureFrequency()
{
    QStringList results;
    for (int channel = 0; channel < 2; ++channel) {
        if (!(channel == 0 ? m_showA->isChecked() : m_showB->isChecked())) continue;
        const QString name = channel == 0 ? "A 通道" : "B 通道";
        // Frequency follows incoming samples even while cursors hold a snapshot.
        // Always use the sampling rate belonging to those latest samples.
        const QVector<double> &data = channel == 0
            ? (m_displayPaused ? m_visibleData : m_lastData)
            : (m_displayPaused ? m_visibleDataB : m_lastDataB);
        const FrequencyEstimate estimate = estimateFrequency(data, m_displayPaused ? m_sampleRateHz : m_latestSampleRateHz);
        if (!estimate.valid) {
            results << name + "：无法可靠测量，请检查采样率及信号是否包含至少两个清晰、稳定的周期。";
            continue;
        }
        results << QString("%1：周期 %2 µs，频率 %3 Hz（约 %4 点/周期）")
            .arg(name).arg(estimate.periodSeconds * 1e6, 0, 'g', 6)
            .arg(estimate.frequencyHz, 0, 'g', 6).arg(estimate.periodSamples, 0, 'g', 5);
    }
    m_frequencyResults = results;
    m_frequencyDirty = false;
    updateFrequencyDisplay();
}

void WaveformWidget::updateFrequencyDisplay()
{
    QString text;
    if (!m_frequencyButton->isChecked()) {
        text = "测频已暂停（保留最近结果）";
        if (!m_frequencyResults.isEmpty()) text += ":\n" + m_frequencyResults.join("\n");
    } else if (m_lastData.isEmpty()) {
        text = "实时测频：等待采集数据。";
    } else if (!m_showA->isChecked() && !m_showB->isChecked()) {
        text = "实时测频：请至少显示一个通道。";
    } else if (m_recordMode) {
        text = "记录测频（冻结记录）：\n" + m_frequencyResults.join("\n");
    } else if (m_displayPaused) {
        text = "测频（波形已暂停）：\n" + m_frequencyResults.join("\n");
    } else {
        text = m_frequencyDataAge.isValid() && m_frequencyDataAge.elapsed() > 1500
            ? "实时测频（等待新数据，保留最近结果）：\n"
            : "实时测频（最新采样）：\n";
        text += m_frequencyResults.join("\n");
    }
    if (m_autoMeasureLabel->text() != text) m_autoMeasureLabel->setText(text);
}

void WaveformWidget::showSpectrum()
{
    if (m_spectrumDialog) {
        m_spectrumDialog->show();
        m_spectrumDialog->raise();
        m_spectrumDialog->activateWindow();
        return;
    }
    QDialog *dialog = new QDialog(this);
    m_spectrumDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("liveSpectrumDialog");
    dialog->setWindowTitle("FFT 频谱");
    dialog->resize(900, 620);
    QVBoxLayout *layout = new QVBoxLayout(dialog);
    QHBoxLayout *controls = new QHBoxLayout();
    controls->addWidget(new QLabel("分析通道：", dialog));
    QComboBox *channelEdit = new QComboBox(dialog);
    channelEdit->setObjectName("comboBox_spectrumChannel");
    channelEdit->addItems({"A 通道", "B 通道"});
    if (rawAnalysisData(m_spectrumChannelIndex,true).isEmpty() && !rawAnalysisData(1-m_spectrumChannelIndex,true).isEmpty())
        m_spectrumChannelIndex = 1-m_spectrumChannelIndex;
    channelEdit->setCurrentIndex(m_spectrumChannelIndex);
    controls->addWidget(channelEdit);
    QPushButton *pause = new QPushButton("暂停频谱", dialog);
    pause->setObjectName("pauseSpectrumButton");
    pause->setCheckable(true);
    pause->setToolTip("只暂停频谱显示，FPGA 采集和 TCP 接收继续");
    controls->addStretch();
    controls->addWidget(pause);
    layout->addLayout(controls);
    QLabel *hint = new QLabel("选择要分析的通道；暂停波形后使用保留的数据，最多采用末尾 4096 个原始采样点。",dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    QChart *chart = new QChart();
    chart->legend()->hide();
    chart->setAnimationOptions(QChart::NoAnimation);
    QLineSeries *line = new QLineSeries();
    chart->addSeries(line);
    QScatterSeries *peak = new QScatterSeries();
    peak->setColor(QColor("#d34d2c"));
    peak->setMarkerSize(12.0);
    chart->addSeries(peak);
    QValueAxis *frequencyAxis = new QValueAxis();
    frequencyAxis->setTitleText("频率 (Hz)");
    frequencyAxis->setRange(0.0, 1.0);
    frequencyAxis->setTickCount(11);
    QValueAxis *levelAxis = new QValueAxis();
    levelAxis->setTitleText("幅度 (dBV RMS)");
    levelAxis->setRange(-120.0, 10.0);
    levelAxis->setTickCount(7);
    chart->addAxis(frequencyAxis, Qt::AlignBottom);
    chart->addAxis(levelAxis, Qt::AlignLeft);
    line->attachAxis(frequencyAxis);
    line->attachAxis(levelAxis);
    peak->attachAxis(frequencyAxis);
    peak->attachAxis(levelAxis);
    layout->addWidget(new QChartView(chart, dialog));
    QLabel *summary = new QLabel(dialog);
    summary->setObjectName("spectrumSummary");
    summary->setWordWrap(true);
    layout->addWidget(summary);
    // 保留图表和曲线对象，每秒最多计算、刷新五次，关闭窗口即停止定时器。
    const auto refresh = [=]() {
        const int channel = channelEdit->currentIndex();
        const QString channelName = analysisChannelName(channel);
        const auto &raw = rawAnalysisData(channel,true);
        const QVector<double> data = probeScaled(raw.mid(std::max(0,raw.size()-4096)),m_probeScale[channel]);
        dialog->setWindowTitle(channelName + (pause->isChecked() ? "FFT 频谱（已暂停）"
            : m_recordMode ? "记录 FFT 频谱" : m_displayPaused ? "暂停波形 FFT 频谱" : "实时 FFT 频谱"));
        dialog->setProperty("displayedChannel", channelName);
        const double rate = m_displayPaused ? m_sampleRateHz : m_latestSampleRateHz;
        const SpectrumResult spectrum = analyzeSpectrum(data, rate);
        if (!spectrum.valid) {
            line->clear();
            peak->clear();
            chart->setTitle(channelName + " FFT · 等待有效波形");
            summary->setText(channelName + "：需要至少 256 个采样点和有效采样率；平直或过弱的信号无法识别主峰。");
            return;
        }
        const double divisor = rate >= 100000000 ? 1e6 : rate >= 100000 ? 1e3 : 1.0;
        const QString unit = divisor == 1e6 ? "MHz" : divisor == 1e3 ? "kHz" : "Hz";
        chart->setTitle(QString(m_recordMode ? "冻结记录 · Hann 窗 · %1 点 FFT · 分辨率 %2 Hz"
                                            : m_displayPaused ? "波形已暂停 · Hann 窗 · %1 点 FFT · 分辨率 %2 Hz"
                                            : "实时 · Hann 窗 · %1 点 FFT · 分辨率 %2 Hz")
            .arg(spectrum.fftSize).arg(spectrum.resolutionHz, 0, 'g', 5) + " · " + channelName);
        frequencyAxis->setTitleText("频率 (" + unit + ")");
        frequencyAxis->setRange(0.0, rate / 2.0 / divisor);
        levelAxis->setRange(-120.0, std::max(10.0, spectrum.peakDbv + 5.0));
        QVector<QPointF> points;
        points.reserve(spectrum.dbv.size());
        for (int bin = 0; bin < spectrum.dbv.size(); ++bin)
            points.append(QPointF(bin * spectrum.resolutionHz / divisor, spectrum.dbv[bin]));
        line->replace(points);
        peak->replace(QVector<QPointF>{QPointF(spectrum.peakFrequencyHz / divisor, spectrum.peakDbv)});
        QString text = channelName + " · 探头 "+probeDescription(channel)+" · " + QString("主峰：%1 Hz，%2 dBV；主峰 2× 位置：%3 dBV；3× 位置：%4 dBV；噪声底近似：%5 dBV。")
            .arg(spectrum.peakFrequencyHz, 0, 'g', 6).arg(spectrum.peakDbv, 0, 'f', 1)
            .arg(spectrum.secondHarmonicDbv, 0, 'f', 1).arg(spectrum.thirdHarmonicDbv, 0, 'f', 1)
            .arg(spectrum.noiseFloorDbv, 0, 'f', 1);
        const FrequencyEstimate fundamental = estimateFrequency(data, rate, m_probeScale[channel]);
        if (fundamental.valid)
            text += QString(" 时域基频估计：%1 Hz。").arg(fundamental.frequencyHz, 0, 'g', 6);
        text += " 有效采样率=原始采样率/采样间隔；噪声底为频谱中位数估计。无新数据时保留最后结果。";
        summary->setText(text);
    };
    QTimer *timer = new QTimer(dialog);
    timer->setInterval(200);
    connect(timer, &QTimer::timeout, dialog, [=]() {
        if (dialog->isVisible() && !pause->isChecked()) refresh();
    });
    connect(channelEdit,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[=](int channel) {
        m_spectrumChannelIndex = channel;
        refresh(); // Switching channels updates once, even when continuous FFT refresh is paused.
    });
    connect(pause, &QPushButton::toggled, dialog, [=](bool paused) {
        pause->setText(paused ? "继续频谱" : "暂停频谱");
        const QString displayed = dialog->property("displayedChannel").toString();
        dialog->setWindowTitle(displayed + (paused ? "FFT 频谱（已暂停）" : "实时 FFT 频谱"));
        if (paused) timer->stop();
        else { refresh(); timer->start(); }
    });
    dialog->show();
    refresh();
    timer->start();
}

void WaveformWidget::setSampleRateHz(double rate)
{
    if (!std::isfinite(rate) || rate < 0.0) rate = 0.0;
    m_latestSampleRateHz = rate;
    m_frequencyDirty = true;
    if (m_displayPaused || m_cursorMode != NoCursor) return; // 与保留的波形使用同一采样率。
    const double first = axisToSample(m_axisX->min()), last = axisToSample(m_axisX->max());
    m_sampleRateHz = rate;
    setSampleRange(first, last);
    updateMeasurements();
}

void WaveformWidget::selectCursorMode(CursorMode mode)
{
    if (m_cursorMode == mode) {
        clearCursors();
        return;
    }
    if (m_visibleData.isEmpty() && m_visibleDataB.isEmpty()) {
        m_measureLabel->setText("暂无波形，请先连接 FPGA 并采集数据。");
        m_timeButton->setChecked(false);
        m_voltageButton->setChecked(false);
        return;
    }
    m_cursorMode = mode;
    m_isDragging = false;
    m_chartView->setCursor(Qt::ArrowCursor);
    m_timeButton->setChecked(mode == TimeCursor);
    m_voltageButton->setChecked(mode == VoltageCursor);
    m_autoScroll = false;
    m_autoFitY = false;
    updateMeasurements();
}

void WaveformWidget::clearCursors()
{
    m_cursorMode = NoCursor;
    if (!m_displayPaused) m_sampleRateHz = m_latestSampleRateHz;
    m_isDragging = false;
    m_chartView->setCursor(Qt::ArrowCursor);
    m_timeButton->setChecked(false);
    m_voltageButton->setChecked(false);
    for (int i = 0; i < 2; ++i) {
        m_hasTimeCursor[i] = false;
        m_hasVoltageCursor[i] = false;
    }
    updateCursorLines();
    m_autoScroll = !m_recordMode && !m_displayPaused;
    if (m_recordMode) { renderRecord(); updateStatistics(); }
    else if (!m_displayPaused && !m_lastData.isEmpty()) setWaveformData(m_lastData, m_lastDataB, 3000);
    updateMeasurements();
    emit displayStateChanged();
}

void WaveformWidget::updateCursorLines()
{
    for (int i = 0; i < 2; ++i) {
        if (m_hasTimeCursor[i]) {
            m_timeCursorLines[i]->replace(QVector<QPointF>{
                QPointF(sampleToAxis(m_timeCursorX[i]), m_axisY->min()),
                QPointF(sampleToAxis(m_timeCursorX[i]), m_axisY->max())});
        } else {
            m_timeCursorLines[i]->clear();
        }
        if (m_hasVoltageCursor[i]) {
            m_voltageCursorLines[i]->replace(QVector<QPointF>{
                QPointF(m_axisX->min(), m_voltageCursorY[i]),
                QPointF(m_axisX->max(), m_voltageCursorY[i])});
        } else {
            m_voltageCursorLines[i]->clear();
        }
    }
}

void WaveformWidget::updateMeasurements()
{
    QStringList parts;
    if (m_hasTimeCursor[0] && m_hasTimeCursor[1]) {
        const double samples = std::abs(m_timeCursorX[1] - m_timeCursorX[0]);
        if (m_sampleRateHz != 0 && samples > 0) {
            const double seconds = samples / double(m_sampleRateHz);
            parts << QString("时间差：%1 µs；若两点相隔一周期，周期：%1 µs，频率：%2 Hz")
                         .arg(seconds * 1e6, 0, 'g', 6).arg(1.0 / seconds, 0, 'g', 6);
        } else {
            parts << QString("时间差：%1 个采样点（采样率未知或光标重合）")
                         .arg(samples, 0, 'f', 0);
        }
    }
    if (m_hasVoltageCursor[0] && m_hasVoltageCursor[1]) {
        parts << QString("电压差：%1 V")
                     .arg(std::abs(m_voltageCursorY[1] - m_voltageCursorY[0]), 0, 'g', 6);
    }
    m_measureLabel->setText(parts.isEmpty()
        ? "光标测量：选择时间或电压光标，在波形区域点击两次。"
        : parts.join("    |    "));
}

void WaveformWidget::autoFit()
{
    clearCursors();
    if (m_recordMode || m_displayPaused) {
        setSampleRange(0, std::max(1, m_visibleData.size() - 1));
        if (m_recordMode) renderRecord();
        fitRecordVoltage(); updateStatistics(); return;
    }
    m_autoFitY = true;
    m_autoScroll = true;
    if (!m_lastData.isEmpty()) setWaveformData(m_lastData, m_lastDataB, 3000);
}

void WaveformWidget::resetZoom()
{
    clearCursors();
    // 坐标轴是用 setRange 缩放的，因此应直接恢复范围。
    setSampleRange(0, m_recordMode || m_displayPaused ? std::max(1, m_visibleData.size() - 1) : 999);
    const double factor=std::max(m_showA->isChecked()?m_probeScale[0]:0.0,m_showB->isChecked()?m_probeScale[1]:0.0);
    m_axisY->setRange(DefaultYMin*(factor>0?factor:1.0), DefaultYMax*(factor>0?factor:1.0));
    m_autoScroll = !m_recordMode && !m_displayPaused;
    m_autoFitY = false;
}

void WaveformWidget::hideEvent(QHideEvent *event)
{
    if (m_cursorMode != NoCursor) clearCursors();
    QWidget::hideEvent(event);
}

bool WaveformWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (obj != m_chartView) {
        return QWidget::eventFilter(obj, event);
    }

    if (event->type() == QEvent::Wheel) {
        QWheelEvent *wheelEvent = static_cast<QWheelEvent *>(event);
        double factor = 1.15;
        if (wheelEvent->angleDelta().y() < 0) factor = 1.0 / factor;
        QPointF mouseValue = m_chart->mapToValue(wheelEvent->pos());

        if (wheelEvent->modifiers() & Qt::ShiftModifier) {
            m_autoFitY = false;
            const double yMin = m_axisY->min();
            const double yMax = m_axisY->max();
            const double oldRange = yMax - yMin;
            const double newRange = qBound(MinYSpan, oldRange / factor, MaxYSpan);
            const double centerY = qBound(yMin, mouseValue.y(), yMax);
            const double ratio = (centerY - yMin) / oldRange;
            const double newMin = centerY - ratio * newRange;
            m_axisY->setRange(newMin, newMin + newRange);
            return true;
        }

        m_autoScroll = false;
        m_autoFitY = false;
        double centerX = mouseValue.x();

        double xMin = m_axisX->min();
        double xMax = m_axisX->max();
        double oldRange = xMax - xMin;
        double newRange = oldRange / factor;

        double ratio = (centerX - xMin) / oldRange;
        double newMin = centerX - ratio * newRange;
        double newMax = newMin + newRange;

        m_axisX->setRange(newMin, newMax);
        return true;
    }

    if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            if (m_cursorMode != NoCursor) {
                if (!m_chart->plotArea().contains(me->pos())) return true;
                const QPointF value = m_chart->mapToValue(me->localPos());
                if (m_cursorMode == TimeCursor) {
                    const int index = m_hasTimeCursor[0] && !m_hasTimeCursor[1] ? 1 : 0;
                    if (index == 0) m_hasTimeCursor[1] = false;
                    m_timeCursorX[index] = std::round(axisToSample(qBound(m_axisX->min(), value.x(), m_axisX->max())));
                    m_hasTimeCursor[index] = true;
                } else {
                    const int index = m_hasVoltageCursor[0] && !m_hasVoltageCursor[1] ? 1 : 0;
                    if (index == 0) m_hasVoltageCursor[1] = false;
                    m_voltageCursorY[index] = qBound(m_axisY->min(), value.y(), m_axisY->max());
                    m_hasVoltageCursor[index] = true;
                }
                updateCursorLines();
                updateMeasurements();
                return true;
            }
            m_isDragging = true;
            m_lastMousePos = me->pos();
            m_chartView->setCursor(Qt::ClosedHandCursor);
            return true;
        }
        if (me->button() == Qt::RightButton) {
            resetZoom();
            return true;
        }
    }

    if (event->type() == QEvent::MouseMove) {
        if (m_isDragging) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            QPoint delta = me->pos() - m_lastMousePos;
            m_lastMousePos = me->pos();

            double xMin = m_axisX->min();
            double xMax = m_axisX->max();
            double range = xMax - xMin;
            double pixelToData = range / m_chartView->width();
            double dataDelta = delta.x() * pixelToData;

            m_axisX->setRange(xMin - dataDelta, xMax - dataDelta);
            m_autoScroll = false;
            m_autoFitY = false;
            return true;
        }
    }

    if (event->type() == QEvent::MouseButtonRelease) {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            m_isDragging = false;
            m_chartView->setCursor(Qt::ArrowCursor);
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}
