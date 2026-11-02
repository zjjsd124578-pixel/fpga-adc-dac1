#ifndef DDS_FREQUENCY_H
#define DDS_FREQUENCY_H
#include <QtGlobal>
#include <cmath>

// Frequencies on the first Nyquist interval; never infer the DDS clock from ADC.
inline bool ddsFrequencyToFtw(double hz, double clockHz, int bits, quint32 &ftw)
{
    if (!std::isfinite(hz) || !std::isfinite(clockHz) || clockHz <= 0 ||
        bits < 1 || bits > 32 || hz < 0 || hz > clockHz / 2.0) return false;
    const double scale = std::ldexp(1.0, bits);
    const double rounded = std::round(hz / clockHz * scale);
    if (rounded < 0 || rounded >= scale) return false;
    ftw = quint32(rounded);
    return true;
}

inline double ddsFtwToFrequency(quint32 ftw, double clockHz, int bits)
{
    return double(ftw) / std::ldexp(1.0, bits) * clockHz;
}

// One complete turn spans 2^32 phase codes. Round in 64 bits before
// wrapping, so 360 degrees maps to zero without a 32-bit overflow.
inline bool ddsDegreesToPhaseWord(double degrees, quint32 &phaseWord)
{
    if (!std::isfinite(degrees) || degrees < 0.0 || degrees > 360.0) return false;
    const quint64 rounded = quint64(std::round(degrees / 360.0 * 4294967296.0));
    phaseWord = quint32(rounded & 0xFFFFFFFFull);
    return true;
}
#endif
