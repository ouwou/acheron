#pragma once

#include <QRandomGenerator>

#include <algorithm>

namespace Acheron {
namespace Discord {

// the official client's backoff: a failure adds a random part of twice the current delay
class Backoff
{
public:
    Backoff(int minMs, int maxMs) : minMs(minMs), maxMs(maxMs), currentMs(minMs) { }

    int fail(int atLeastMs = 0)
    {
        failCount++;
        const int added = static_cast<int>(2.0 * currentMs * QRandomGenerator::global()->generateDouble());
        currentMs = std::min(currentMs + added, maxMs);
        return std::max(currentMs, atLeastMs);
    }

    void succeed()
    {
        failCount = 0;
        currentMs = minMs;
    }

    [[nodiscard]] int failures() const { return failCount; }

private:
    int minMs;
    int maxMs;
    int currentMs;
    int failCount = 0;
};

} // namespace Discord
} // namespace Acheron
