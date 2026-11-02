#ifndef WAVEFORM_STATISTICS_H
#define WAVEFORM_STATISTICS_H

#include <QVector>
#include <algorithm>
#include <cmath>

struct WaveformStatistics {
    bool valid = false;
    int count = 0;
    double mean = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double peakToPeak = 0.0;
    double rms = 0.0;
};

inline WaveformStatistics calculateWaveformStatistics(
    const QVector<double> &samples, int first, int last)
{
    WaveformStatistics result;
    if (samples.isEmpty()) return result;
    first = std::max(0, first);
    last = std::min(samples.size() - 1, last);
    if (first > last) return result;
    double sum = 0.0;
    double sumSquares = 0.0;
    result.minimum = samples[first];
    result.maximum = samples[first];
    for (int i = first; i <= last; ++i) {
        const double value = samples[i];
        if (!std::isfinite(value)) return WaveformStatistics();
        sum += value;
        sumSquares += value * value;
        result.minimum = std::min(result.minimum, value);
        result.maximum = std::max(result.maximum, value);
    }
    result.count = last - first + 1;
    result.mean = sum / result.count;
    result.peakToPeak = result.maximum - result.minimum;
    result.rms = std::sqrt(sumSquares / result.count);
    result.valid = true;
    return result;
}

#endif // WAVEFORM_STATISTICS_H
