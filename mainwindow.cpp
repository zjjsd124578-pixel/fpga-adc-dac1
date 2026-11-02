#include "mainwindow.h"
#include "homewidget.h"
#include "adcwidget.h"
#include "dacwidget.h"
#include "app_style.h"
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>
#include <algorithm>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // 1. 创建多路选择器，赋值给成员变量 m_stack
    m_stack = new QStackedWidget(this);
    setCentralWidget(m_stack);

    // 2. 实例化三个界面
    m_home = new HomeWidget(this);
    m_adc  = new AdcWidget(this);
    m_dac  = new DacWidget(this, m_adc);

    // 3. 把它们加入选择器
    m_stack->addWidget(m_home); // index 0
    m_stack->addWidget(m_adc);  // index 1
    m_stack->addWidget(m_dac);  // index 2

    // 4. 首页按钮 -> 切换二级界面
    connect(m_home, &HomeWidget::requestAdc, this, [this](){
        if (m_closedLoopDialog) m_closedLoopDialog->close();
        m_stack->setCurrentWidget(m_adc);
    });
    connect(m_home, &HomeWidget::requestDac, this, [=](){
        if (m_closedLoopDialog) m_closedLoopDialog->close();
        m_dac->setAmplitudeLoopMode(false);
        m_dac->setSweepControlsVisible(true);
        m_stack->setCurrentWidget(m_dac);
    });

    // 5. 二级界面的返回按钮 -> 切回首页
    connect(m_adc, &AdcWidget::requestBack, this, [=](){ m_stack->setCurrentWidget(m_home); });
    connect(m_dac, &DacWidget::requestBack, this, [=](){
        if (m_closedLoopDialog) m_closedLoopDialog->close();
        else m_stack->setCurrentWidget(m_home);
    });
    connect(m_adc, &AdcWidget::requestClosedLoopControl, this, [this]() { openClosedLoopControl(); });
    connect(m_home, &HomeWidget::requestAmplitudeLoop, this, [this]() { openClosedLoopControl(true); });
    connect(m_home, &HomeWidget::requestCircuitTest, this, [this]() { openClosedLoopControl(false); });
    connect(m_home, &HomeWidget::requestDigitalPll, this, [this]() { openClosedLoopControl(false, true); });

    // 6. 启动时默认显示首页
    m_stack->setCurrentWidget(m_home);

    setMinimumSize(1150, 720);
    resize(1280, 800);
    setWindowTitle("FPGA 信号采集与测试平台");
}

MainWindow::~MainWindow() {}

void MainWindow::openClosedLoopControl(bool amplitudeLoop, bool digitalPll)
{
    m_stack->setCurrentWidget(m_adc);
    m_dac->setCircuitTestMode(false);
    m_dac->setDigitalPllMode(false);
    if (amplitudeLoop) m_dac->selectAmplitudeLoopMode();
    else m_dac->setAmplitudeLoopMode(false);
    m_dac->setSweepControlsVisible(false);
    if (digitalPll) m_dac->setDigitalPllMode(true);
    if (!amplitudeLoop && !digitalPll) m_dac->setCircuitTestMode(true);
    const QString title = digitalPll ? "数字 PLL · ADC A 参考 / DAC 跟随"
        : amplitudeLoop ? "幅值闭环测试 · DAC 输出控制" : "待测电路测试 · 幅频 / 相频 / −3 dB";
    const QString signalPath = digitalPll ? "信号路径：外部参考 → ADC A → 数字 PLL → DAC 输出；ADC B 可观察输出"
        : amplitudeLoop ? "信号路径：DAC 输出 → 待测电路（或直接连接）→ ADC 输入"
        : "信号路径：DAC → 待测电路；ADC A 测输入参考，ADC B 测输出响应";
    const QString hintText = digitalPll
        ? "先接入参考，再配置并启动 PLL。参考丢失时 DAC 可能静音；关闭窗口不停止板端 PLL，切回其他模式会先确认 PLL 停用。"
        : amplitudeLoop
        ? "设置目标峰峰值并下发配置，由板端根据 ADC 反馈自动调幅；可查询目标、实测值及调节状态。关闭窗口不停止输出或闭环。"
        : "先直通校正，再测试低通、高通或带通；查看增益、相位、−3 dB 点、带宽和 Q 值。关闭窗口不会停止正在进行的测试。";
    if (m_closedLoopDialog) {
        m_closedLoopDialog->setWindowTitle(title);
        m_closedLoopDialog->findChild<QLabel *>("label_testPath")->setText(signalPath);
        m_closedLoopDialog->findChild<QLabel *>("label_testHint")->setText(hintText);
        m_closedLoopDialog->findChild<QLabel *>("label_testPath")->setVisible(amplitudeLoop || digitalPll);
        m_closedLoopDialog->findChild<QLabel *>("label_testHint")->setVisible(amplitudeLoop || digitalPll);
        m_closedLoopDialog->show();
        m_closedLoopDialog->raise();
        m_closedLoopDialog->activateWindow();
        return;
    }
    QDialog *dialog = new QDialog(this, Qt::Window);
    m_closedLoopDialog = dialog;
    dialog->setObjectName("closedLoopControlDialog");
    dialog->setWindowTitle(title);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(false);
    dialog->resize(760, 800);
    dialog->setMinimumSize(700, 720);
    dialog->setStyleSheet(signalAnalyzerStyle() + "QDialog#closedLoopControlDialog { background: #f3f6fa; }");
    QVBoxLayout *layout = new QVBoxLayout(dialog);
    QLabel *path = new QLabel(signalPath, dialog);
    path->setObjectName("label_testPath");
    path->setWordWrap(true);
    path->setStyleSheet("color: #2869d8; padding: 8px; font-weight: bold;");
    layout->addWidget(path);
    QLabel *hint = new QLabel(hintText, dialog);
    hint->setObjectName("label_testHint");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    path->setVisible(amplitudeLoop || digitalPll);
    hint->setVisible(amplitudeLoop || digitalPll);
    // Reuse the existing editor and shared socket; opening never sends commands.
    m_stack->removeWidget(m_dac);
    layout->addWidget(m_dac, 1);
    m_dac->show();
    connect(dialog, &QDialog::finished, this, [this]() {
        m_dac->setCircuitTestMode(false);
        m_dac->setDigitalPllMode(false);
        m_dac->setAmplitudeLoopMode(false);
        m_dac->setSweepControlsVisible(true);
        m_dac->setParent(m_stack);
        m_stack->addWidget(m_dac);
        m_closedLoopDialog = nullptr;
    });
    dialog->show();
    QScreen *screen = windowHandle() ? windowHandle()->screen() : QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        const int leftMin = std::max(minimumWidth(), minimumSizeHint().width());
        const int rightMin = std::max(dialog->minimumWidth(), dialog->minimumSizeHint().width());
        const int requiredHeight = std::max(minimumSizeHint().height(), dialog->minimumSizeHint().height());
        if (area.width() >= leftMin + rightMin + 40 && area.height() >= requiredHeight + 48) {
            if (isMaximized()) showNormal();
            const int leftWidth = area.width() - rightMin - 40;
            resize(leftWidth, area.height() - 48);
            move(area.left() + 8, area.top() + 30);
            dialog->resize(rightMin, area.height() - 48);
            dialog->move(area.left() + leftWidth + 24, area.top() + 30);
        } else {
            dialog->move(std::max(area.left(), area.right() - dialog->width() - 16), area.top() + 30);
        }
    }
}
