#include "homewidget.h"
#include "ui_homewidget.h"
#include "app_style.h"
#include <QPainter>
#include <QPainterPath>
#include <cmath>
#include <algorithm>
#include <QFrame>
#include <QLabel>

// Decorative waveforms on the start page; these are not live measurements.
class HomeWavePreview : public QWidget
{
public:
    HomeWavePreview(bool dualChannel, QWidget *parent) : QWidget(parent), m_dual(dualChannel)
    {
        setFixedHeight(58);
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area = rect().adjusted(1, 1, -1, -1);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#f3f7fd"));
        painter.drawRoundedRect(area, 8, 8);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor("#e2eaf6"), 1));
        for (int i = 1; i < 8; ++i)
            painter.drawLine(QPointF(area.width()*i/8, 12), QPointF(area.width()*i/8, height()-12));
        painter.drawLine(QPointF(14, height()/2), QPointF(width()-14, height()/2));
        for (int channel = 0; channel < (m_dual ? 2 : 1); ++channel) {
            QPainterPath trace;
            for (int i = 0; i <= 400; ++i) {
                const double t = i/400.0;
                const QPointF point(14 + t*(width()-28), height()/2.0 -
                    (channel ? 13 : 19)*std::sin(t*6.28318530718*3 + channel*0.9));
                if (!i) trace.moveTo(point); else trace.lineTo(point);
            }
            painter.setPen(QPen(QColor(channel ? "#e58a23" : "#2869d8"), 2));
            painter.drawPath(trace);
        }
    }
private:
    bool m_dual;
};

// Signal-path illustrations describe the experiment, not live board status.
class HomeSignalPath : public QWidget
{
public:
    HomeSignalPath(bool feedback, QWidget *parent) : QWidget(parent), m_feedback(feedback)
    {
        setFixedHeight(58);
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#f3f7fd"));
        painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 8, 8);
        const double nodeWidth = std::min(120.0, (width() - 100.0) / 3.0);
        const double gap = (width() - 28.0 - 3.0 * nodeWidth) / 2.0;
        const QStringList names = m_feedback ? QStringList{"DAC 输出", "ADC 反馈", "幅值调节"}
                                             : QStringList{"DAC 激励", "待测电路", "ADC 响应"};
        const double nodeY = m_feedback ? 7.0 : 15.0;
        auto arrow = [&](double from, double to, double y) {
            painter.drawLine(QPointF(from, y), QPointF(to, y));
            const double direction = to > from ? 1.0 : -1.0;
            painter.drawLine(QPointF(to, y), QPointF(to - direction * 5, y - 3));
            painter.drawLine(QPointF(to, y), QPointF(to - direction * 5, y + 3));
        };
        for (int i = 0; i < 3; ++i) {
            const QRectF node(14.0 + i * (nodeWidth + gap), nodeY, nodeWidth, 27.0);
            painter.setPen(QPen(QColor("#ceddf1"), 1));
            painter.setBrush(Qt::white);
            painter.drawRoundedRect(node, 5, 5);
            painter.setPen(QColor("#385779"));
            painter.drawText(node, Qt::AlignCenter, names.at(i));
            if (i < 2) {
                painter.setPen(QPen(QColor("#2869d8"), 1.5));
                arrow(node.right() + 6, node.right() + gap - 6, node.center().y());
            }
        }
        if (m_feedback) {
            painter.setPen(QPen(QColor("#2869d8"), 1.3));
            const double left = 14.0 + nodeWidth / 2.0;
            const double right = 14.0 + 2 * (nodeWidth + gap) + nodeWidth / 2.0;
            painter.drawLine(QPointF(right, nodeY + 27), QPointF(right, 48));
            arrow(right, left, 48);
            painter.drawLine(QPointF(left, 48), QPointF(left, nodeY + 27));
        }
    }
private:
    bool m_feedback;
};

HomeWidget::HomeWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::HomeWidget)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(signalAnalyzerStyle() + QStringLiteral(
        "QLabel#label { font-size: 36px; font-weight: bold; color: #182b46; }"
        "QLabel#label_homeSubtitle { font-size: 15px; }"
        "QLabel[sectionTitle='true'] { font-size: 17px; font-weight: bold; color: #234369; }"
        "QLabel[capability='true'] { font-size: 12px; color: #526781; background: #f0f5fc; border-radius: 4px; padding: 3px 8px; }"
        "QWidget#HomeWidget QPushButton { min-height: 22px; font-size: 14px; }"));
    ui->adcCardLayout->insertWidget(0, new HomeWavePreview(true, ui->card_adc));
    ui->dacCardLayout->insertWidget(0, new HomeWavePreview(false, ui->card_dac));
    ui->loopCardLayout->insertWidget(0, new HomeSignalPath(true, ui->card_loop));
    ui->circuitCardLayout->insertWidget(0, new HomeSignalPath(false, ui->card_circuit));
    for (auto cardLayout : {ui->adcCardLayout, ui->dacCardLayout, ui->loopCardLayout, ui->circuitCardLayout}) {
        cardLayout->setContentsMargins(18,12,18,12);
        cardLayout->setSpacing(4);
    }
    connect(ui->btn_adc, &QPushButton::clicked, this, &HomeWidget::requestAdc);
    connect(ui->btn_amplitudeLoop, &QPushButton::clicked, this, &HomeWidget::requestAmplitudeLoop);
    connect(ui->btn_dac, &QPushButton::clicked, this, &HomeWidget::requestDac);
    connect(ui->btn_circuitTest, &QPushButton::clicked, this, &HomeWidget::requestCircuitTest);
    auto pllCard = new QFrame(this);
    pllCard->setProperty("card", true);
    auto pllLayout = new QHBoxLayout(pllCard);
    pllLayout->setContentsMargins(18, 10, 18, 10);
    auto pllDescription = new QLabel("<b>数字 PLL</b>　ADC A 参考 → DAC 跟随 · 频率与相位锁定", pllCard);
    pllDescription->setWordWrap(true);
    pllLayout->addWidget(pllDescription, 1);
    auto pllEntry = new QPushButton("进入数字 PLL", pllCard);
    pllEntry->setObjectName("btn_digitalPll");
    pllEntry->setProperty("primary", true);
    pllLayout->addWidget(pllEntry);
    ui->homePageLayout->addWidget(pllCard);
    connect(pllEntry, &QPushButton::clicked, this, &HomeWidget::requestDigitalPll);

}

HomeWidget::~HomeWidget()
{
    delete ui;
}
