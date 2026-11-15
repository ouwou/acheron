#pragma once

#include <QtGlobal>

#include <cmath>

namespace Acheron {
namespace Core {
namespace Audio {

// the official client's volume sliders: up to the middle the gain is the position to the power 2.8, which the ear hears as
// even steps, and the upper half adds up to 6 dB
namespace VolumeCurve {

constexpr int UNCHANGED_POSITION = 100;
constexpr int MAX_POSITION = 200;
constexpr double QUIET_EXPONENT = 2.8;
constexpr double MAX_BOOST_DB = 6.0;

[[nodiscard]] inline float gainAt(int position)
{
    if (position <= 0)
        return 0.0f;
    const double ratio = static_cast<double>(position) / UNCHANGED_POSITION;
    return static_cast<float>(ratio < 1.0 ? std::pow(ratio, QUIET_EXPONENT) : std::pow(10.0, (ratio - 1.0) * MAX_BOOST_DB / 20.0));
}

[[nodiscard]] inline int positionOf(float gain)
{
    if (gain <= 0.0f)
        return 0;
    const double ratio = gain < 1.0f ? std::pow(gain, 1.0 / QUIET_EXPONENT) : 20.0 * std::log10(gain) / MAX_BOOST_DB + 1.0;
    return qBound(0, qRound(ratio * UNCHANGED_POSITION), MAX_POSITION);
}

} // namespace VolumeCurve

} // namespace Audio
} // namespace Core
} // namespace Acheron
