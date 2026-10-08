#pragma once

#include <algorithm>

namespace Acheron {
namespace Core {

[[nodiscard]] inline int normalizedFrameDelayMs(int delayMs)
{
    if (delayMs <= 10)
        return 100;
    return std::max(delayMs, 20);
}

} // namespace Core
} // namespace Acheron
