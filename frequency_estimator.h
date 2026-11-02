#ifndef FREQUENCY_ESTIMATOR_H
#define FREQUENCY_ESTIMATOR_H

#include <QVector>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

// 从波形的上升中点交叉估计周期。要求至少两个一致的完整周期。
struct FrequencyEstimate {
    bool valid = false;
    double periodSamples = 0.0;
    double periodSeconds = 0.0;
    double frequencyHz = 0.0;
};

inline FrequencyEstimate estimateFrequency(const QVector<double> &samples, double sampleRateHz, double voltageScale = 1.0)
{
    FrequencyEstimate result;
    if (!std::isfinite(sampleRateHz) || sampleRateHz <= 0 || samples.size() < 12
        || !std::isfinite(voltageScale) || voltageScale <= 0) return result;
    const auto limits = std::minmax_element(samples.cbegin(), samples.cend());
    const double lowValue = *limits.first;
    const double highValue = *limits.second;
    const double amplitude = highValue - lowValue;
    if (!std::isfinite(amplitude) || amplitude < 0.02 * voltageScale) return result;

    const double middle = (lowValue + highValue) * 0.5;
    const double rearmLevel = middle - amplitude * 0.20;
    QVector<double> crossings;
    bool armed = samples[0] <= rearmLevel;
    for (int i = 1; i < samples.size(); ++i) {
        const double previous = samples[i - 1];
        const double current = samples[i];
        if (!std::isfinite(previous) || !std::isfinite(current)) return result;
        if (current <= rearmLevel) armed = true;
        if (armed && previous <= middle && current > middle) {
            const double fraction = (middle - previous) / (current - previous);
            crossings.append(double(i - 1) + fraction);
            armed = false;
        }
    }
    if (crossings.size() < 3) return result;

    QVector<double> periods;
    for (int i = 1; i < crossings.size(); ++i)
        periods.append(crossings[i] - crossings[i - 1]);
    std::sort(periods.begin(), periods.end());
    const double median = periods[periods.size() / 2];
    if (median < 4.0) return result; // 每周期少于四点时不可靠。
    double sum = 0.0;
    int consistent = 0;
    for (double period : periods) {
        if (std::abs(period - median) > median * 0.15) return result;
        sum += period;
        ++consistent;
    }
    if (consistent < 2) return result;
    result.periodSamples = sum / consistent;
    result.periodSeconds = result.periodSamples / double(sampleRateHz);
    result.frequencyHz = 1.0 / result.periodSeconds;
    result.valid = true;
    return result;
}

#endif // FREQUENCY_ESTIMATOR_H
