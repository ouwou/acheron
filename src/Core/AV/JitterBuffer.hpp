#pragma once

#include <QByteArray>
#include <QHash>

#include <cstdint>

namespace Acheron {
namespace Core {
namespace AV {

class JitterBuffer
{
public:
    explicit JitterBuffer(int capacity = 10);

    /// Add a frame to the buffer.
    void push(uint16_t sequence, const QByteArray &data);

    /// Get the next frame in sequence order.
    /// Returns empty QByteArray if the frame is missing (packet loss).
    QByteArray pop();

    /// Reset the buffer state.
    void reset();

    /// Returns true once the buffer has pre-buffered enough frames for playback.
    [[nodiscard]] bool isReady() const { return initialized && !prebuffering; }

private:
    /// Returns true if sequence a is "newer" than b (handles 16-bit wraparound).
    static bool seqNewer(uint16_t a, uint16_t b);

    QHash<uint16_t, QByteArray> frames;
    uint16_t nextSequence = 0;
    bool initialized = false;
    int capacity;

    int targetDelay = 3;
    bool prebuffering = true;
    int consecutiveMisses = 0;
    int consecutiveHits = 0;

    static constexpr int MAX_CONSECUTIVE_MISSES = 25;
};

} // namespace AV
} // namespace Core
} // namespace Acheron
