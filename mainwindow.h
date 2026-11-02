#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStackedWidget>
#include <QPointer>
class QDialog;

class HomeWidget;
class AdcWidget;
class DacWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    void openClosedLoopControl(bool amplitudeLoop = false, bool digitalPll = false);
    QPointer<QDialog> m_closedLoopDialog;
    QStackedWidget *m_stack; // 成员变量声明
    HomeWidget     *m_home;
    AdcWidget      *m_adc;
    DacWidget      *m_dac;
};

#endif // MAINWINDOW_H
