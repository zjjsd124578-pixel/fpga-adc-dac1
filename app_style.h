#ifndef APP_STYLE_H
#define APP_STYLE_H
#include <QString>
inline QString signalAnalyzerStyle()
{
    return QStringLiteral(
        "QWidget { font-family: 'Microsoft YaHei'; font-size: 13px; color: #243247; }"
        "QWidget#AdcWidget, QWidget#DacWidget, QWidget#HomeWidget { background: #f3f6fa; }"
        "QGroupBox { background: white; border: 1px solid #dce4ee; border-radius: 10px; margin-top: 18px; padding: 12px; font-weight: bold; }"
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 12px; padding: 0 5px; color: #234369; }"
        "QLabel { background: transparent; border: none; font-weight: normal; }"
        "QLabel[heading='true'] { font-size: 24px; font-weight: bold; color: #182b46; }"
        "QLabel[muted='true'] { color: #69788d; }"
        "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: white; border: 1px solid #cfd9e6; border-radius: 5px; padding: 5px 6px; min-height: 20px; font-weight: normal; }"
        "QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #3679df; }"
        "QPushButton { background: white; border: 1px solid #cfd9e6; border-radius: 6px; padding: 6px 10px; min-height: 20px; font-weight: normal; }"
        "QPushButton:hover { background: #edf3fc; border-color: #8aaddc; }"
        "QPushButton:pressed, QPushButton:checked { background: #dce9fc; color: #2869d8; border-color: #8aaddc; }"
        "QPushButton[primary='true'] { background: #2869d8; color: white; border-color: #2869d8; font-weight: bold; }"
        "QPushButton[primary='true']:hover { background: #1e58bb; }"
        "QPushButton:disabled, QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { background: #e8edf4; color: #98a4b4; border-color: #dce4ee; }"
        "QPlainTextEdit { background: #f8fafd; border: 1px solid #e1e8f1; border-radius: 6px; padding: 6px; font-family: 'Consolas', 'Microsoft YaHei'; font-size: 12px; font-weight: normal; }"
        "QCheckBox { spacing: 6px; background: transparent; font-weight: normal; }"
        "QScrollArea { border: none; background: transparent; }"
        "QWidget#adcSidebar { background: transparent; }"
        "QScrollBar:vertical { background: #edf2f8; width: 8px; margin: 0; }"
        "QScrollBar::handle:vertical { background: #c4d2e4; border-radius: 4px; min-height: 30px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QFrame[card='true'] { background: white; border: 1px solid #dce4ee; border-radius: 12px; }"
        "QLabel[cardTitle='true'] { font-size: 22px; font-weight: bold; color: #234369; }"
        "QLabel[eyebrow='true'] { color: #2869d8; font-size: 12px; font-weight: bold; }"
    );
}
#endif
