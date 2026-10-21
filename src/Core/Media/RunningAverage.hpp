#pragma once

#include <QtGlobal>

#include <algorithm>

namespace Acheron {
namespace Core {
namespace Media {

inline constexpr double AVERAGED_SAMPLES = 30;

// over about the last AVERAGED_SAMPLES samples; until there are that many, over all of them
[[nodiscard]] inline double withSample(double average, double sample, quint64 samplesSoFar)
{
    return average + (sample - average) / std::clamp<double>(static_cast<double>(samplesSoFar), 1, AVERAGED_SAMPLES);
}

} // namespace Media
} // namespace Core
} // namespace Acheron
