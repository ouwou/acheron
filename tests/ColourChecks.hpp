#pragma once

#include <cmath>

namespace ColourChecks {

struct Yuv
{
    double y = 0;
    double u = 0;
    double v = 0;
};

inline Yuv inYuv(int red, int green, int blue, bool bt709, bool fullRange = false)
{
    const double kr = bt709 ? 0.2126 : 0.299;
    const double kb = bt709 ? 0.0722 : 0.114;
    const double luma = kr * red + (1 - kr - kb) * green + kb * blue;
    const double u = (blue - luma) / (2 * (1 - kb));
    const double v = (red - luma) / (2 * (1 - kr));
    if (fullRange)
        return { luma, 128 + u, 128 + v };
    return { 16 + 219 * luma / 255, 128 + 224 * u / 255, 128 + 224 * v / 255 };
}

inline double distance(const Yuv &a, const Yuv &b)
{
    return std::sqrt((a.y - b.y) * (a.y - b.y) + (a.u - b.u) * (a.u - b.u) + (a.v - b.v) * (a.v - b.v));
}

} // namespace ColourChecks
