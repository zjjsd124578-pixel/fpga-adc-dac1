#ifndef SPECTRUM_ANALYZER_H
#define SPECTRUM_ANALYZER_H

#include <QVector>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <complex>

struct SpectrumResult {
    bool valid = false;
    int fftSize = 0;
    double resolutionHz = 0.0;
    double peakFrequencyHz = 0.0;
    double peakDbv = 0.0;
    double secondHarmonicDbv = -120.0;
    double thirdHarmonicDbv = -120.0;
    double noiseFloorDbv = -120.0;
    QVector<double> dbv;
    QVector<double> phaseDegrees;
    int peakBin = 0;
};

inline SpectrumResult analyzeSpectrum(const QVector<double> &samples, double sampleRateHz)
{
    SpectrumResult result;
    if (!std::isfinite(sampleRateHz) || sampleRateHz <= 0 || samples.size() < 256) return result;
    int size = 1;
    while (size * 2 <= samples.size() && size < 4096) size *= 2;
    result.fftSize = size;
    result.resolutionHz = double(sampleRateHz) / size;
    const int start = samples.size() - size;
    double mean = 0.0;
    for (int i = start; i < samples.size(); ++i) {
        if (!std::isfinite(samples[i])) return result;
        mean += samples[i];
    }
    mean /= size;

    const double pi = 3.14159265358979323846;
    QVector<std::complex<double>> spectrum(size);
    double windowSum = 0.0;
    for (int i = 0; i < size; ++i) {
        const double window = 0.5 - 0.5 * std::cos(2.0 * pi * i / (size - 1));
        spectrum[i] = std::complex<double>((samples[start + i] - mean) * window, 0.0);
        windowSum += window;
    }
    // 原位 radix-2 FFT。
    for (int i = 1, j = 0; i < size; ++i) {
        int bit = size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(spectrum[i], spectrum[j]);
    }
    for (int length = 2; length <= size; length <<= 1) {
        const double angle = -2.0 * pi / length;
        const std::complex<double> rotation(std::cos(angle), std::sin(angle));
        for (int base = 0; base < size; base += length) {
            std::complex<double> multiplier(1.0, 0.0);
            for (int offset = 0; offset < length / 2; ++offset) {
                const auto even = spectrum[base + offset];
                const auto odd = spectrum[base + offset + length / 2] * multiplier;
                spectrum[base + offset] = even + odd;
                spectrum[base + offset + length / 2] = even - odd;
                multiplier *= rotation;
            }
        }
    }

    // dBV：相对于 1 V RMS。Hann 窗校正正弦幅度，底限 -120 dBV。
    result.dbv.resize(size / 2 + 1);
    result.phaseDegrees.resize(size / 2 + 1);
    int peakBin = 1;
    for (int bin = 0; bin <= size / 2; ++bin) {
        const double peakVolts = 2.0 * std::abs(spectrum[bin]) / windowSum;
        const double rmsVolts = peakVolts / std::sqrt(2.0);
        result.dbv[bin] = 20.0 * std::log10(std::max(rmsVolts, 1e-6));
        // Cosine-reference phase at the first sample of this FFT segment.
        result.phaseDegrees[bin] = std::arg(spectrum[bin]) * 180.0 / pi;
        if (bin > 0 && bin < size / 2 && result.dbv[bin] > result.dbv[peakBin])
            peakBin = bin;
    }
    result.peakFrequencyHz = peakBin * result.resolutionHz;
    result.peakBin = peakBin;
    result.peakDbv = result.dbv[peakBin];
    if (result.peakDbv <= -100.0) return result;
    // 用对数幅度的抛物线插值，改善主峰频率的分辨率。
    if (peakBin > 1 && peakBin + 1 < size / 2) {
        const double left = result.dbv[peakBin - 1];
        const double center = result.dbv[peakBin];
        const double right = result.dbv[peakBin + 1];
        const double divisor = left - 2.0 * center + right;
        if (divisor < -1e-9) {
            const double correction = std::max(-0.5, std::min(0.5,
                0.5 * (left - right) / divisor));
            result.peakFrequencyHz = (peakBin + correction) * result.resolutionHz;
        }
    }
    auto harmonicLevel = [&](int multiple) {
        const int bin = int(std::round(result.peakFrequencyHz * multiple / result.resolutionHz));
        if (bin >= size / 2) return -120.0;
        double level = -120.0;
        for (int candidate = std::max(1, bin - 1);
             candidate <= std::min(size / 2 - 1, bin + 1); ++candidate)
            level = std::max(level, result.dbv[candidate]);
        return level;
    };
    result.secondHarmonicDbv = harmonicLevel(2);
    result.thirdHarmonicDbv = harmonicLevel(3);
    QVector<double> noise;
    noise.reserve(size / 2);
    for (int bin = 1; bin < size / 2; ++bin) {
        bool nearPeak = false;
        for (int multiple = 1; multiple <= 3; ++multiple) {
            const int center = int(std::round(result.peakFrequencyHz * multiple / result.resolutionHz));
            if (std::abs(bin - center) <= 3) nearPeak = true;
        }
        if (!nearPeak) noise.append(result.dbv[bin]);
    }
    if (!noise.isEmpty()) {
        std::sort(noise.begin(), noise.end());
        result.noiseFloorDbv = noise[noise.size() / 2];
    }
    result.valid = true;
    return result;
}

#endif // SPECTRUM_ANALYZER_H
