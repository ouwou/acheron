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

    [[nodiscard]] bool isActive() const { return initialized; }
    [[nodiscard]] int size() const { return frames.size(); }

private:
    /// Returns true if sequence a is "newer" than b (handles 16-bit wraparound).
    static bool seqNewer(uint16_t a, uint16_t b);

    QHash<uint16_t, QByteArray> frames;
    uint16_t nextSequence = 0;
    bool initialized = false;
    int capacity;
};

} // namespace AV
} // namespace Core
} // namespace Acheron
