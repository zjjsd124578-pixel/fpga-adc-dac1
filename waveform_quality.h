#ifndef WAVEFORM_QUALITY_H
#define WAVEFORM_QUALITY_H
#include <QVector>
#include <QStringList>
#include <cmath>
#include "frequency_estimator.h"

struct WaveformRailCheck {
    bool finite = true;
    int railPoints = 0;
};

inline WaveformRailCheck checkWaveformRails(const QVector<double> &data, double probeScale = 1.0)
{
    WaveformRailCheck check;
    if (!std::isfinite(probeScale) || probeScale <= 0) probeScale = 1.0;
    for (double value : data) {
        if (!std::isfinite(value)) { check.finite = false; continue; }
        const double adcValue = value / probeScale;
        // Within one ADC code of either 12-bit AN9238 full-scale endpoint.
        if (adcValue <= -5.0 + 10.0 / 4096.0 || adcValue >= 5.0 - 20.0 / 4096.0)
            ++check.railPoints;
    }
    return check;
}

inline QStringList waveformQualityWarnings(const QVector<double> &data, double rate,
                                          const QStringList &acquisition, double probeScale = 1.0)
{
    QStringList warnings = acquisition;
    bool hasDmaStatus = false;
    for (const QString &warning : warnings)
        if (warning.startsWith("DMA ")) hasDmaStatus = true;
    if (!hasDmaStatus)
        warnings << "DMA 状态未确认：设备溢出标志未提供，不能判定正常。";
    const WaveformRailCheck check = checkWaveformRails(data, probeScale);
    if (!check.finite) warnings << "无效数据：存在非有限采样值，不能解释为正常波形。";
    if (check.railPoints > 0)
        warnings << QString("疑似 ADC 削顶：%1 个采样点接近 ADC 端 ±5 V 量程端点，可能饱和；需核对原始码值及实际输入。")
            .arg(check.railPoints);
    if (!(rate > 0) || !std::isfinite(rate))
        warnings << "时间基准无效：有效采样率未知，禁止给出可靠频率或周期结论。";
    if (data.size() < 256)
        warnings << QString("历史数据不足：仅 %1 点，低于分析参考下限 256 点；不宜判断频谱和稳定性。")
            .arg(data.size());
    if (check.finite && rate > 0 && std::isfinite(rate)) {
        const FrequencyEstimate frequency = estimateFrequency(data, rate, probeScale);
        if (!frequency.valid)
            warnings << "周期依据不足：未获得稳定的周期估计；可能是历史覆盖不足、直流或非周期信号，不能直接判为异常或正常。";
        else if (double(data.size() - 1) * frequency.frequencyHz / rate < 3.0)
            warnings << "历史覆盖不足：当前快照少于 3 个估计周期，周期稳定性结论不可靠。";
    }
    warnings.removeDuplicates();
    return warnings;
}
#endif
