#include "JitterBuffer.hpp"

namespace Acheron {
namespace Core {
namespace AV {

bool JitterBuffer::seqNewer(uint16_t a, uint16_t b)
{
    return static_cast<int16_t>(a - b) > 0;
}

JitterBuffer::JitterBuffer(int capacity)
    : capacity(capacity)
{
}

void JitterBuffer::push(uint16_t sequence, const QByteArray &data)
{
    if (!initialized) {
        nextSequence = sequence;
        initialized = true;
    }

    // If the sequence is way ahead of what we expect, the speaker resumed
    // after a long silence. Reset and resync.
    if (seqNewer(sequence, static_cast<uint16_t>(nextSequence + capacity * 10))) {
        reset();
        nextSequence = sequence;
        initialized = true;
    }

    // Discard if too old (already played or beyond buffer capacity)
    if (seqNewer(nextSequence, sequence))
        return;

    frames.insert(sequence, data);

    // Evict frames that are too far behind the playback pointer
    QList<uint16_t> stale;
    for (auto it = frames.begin(); it != frames.end(); ++it) {
        if (seqNewer(nextSequence, static_cast<uint16_t>(it.key() + capacity)))
            stale.append(it.key());
    }
    for (uint16_t seq : stale)
        frames.remove(seq);

    if (prebuffering && frames.size() >= targetDelay)
        prebuffering = false;
}

QByteArray JitterBuffer::pop()
{
    if (!initialized)
        return {};

    if (prebuffering)
        return {};

    QByteArray data = frames.take(nextSequence);
    nextSequence++;

    if (!data.isEmpty()) {
        consecutiveHits++;
        consecutiveMisses = 0;

        if (consecutiveHits >= 200 && targetDelay > 2) {
            targetDelay--;
            consecutiveHits = 0;
        }
    } else {
        consecutiveMisses++;
        consecutiveHits = 0;

        if (consecutiveMisses >= 3 && targetDelay < 8) {
            targetDelay++;
            prebuffering = true;
            consecutiveMisses = 0;
        }

        if (consecutiveMisses >= MAX_CONSECUTIVE_MISSES) {
            reset();
        }
    }

    return data;
}

void JitterBuffer::reset()
{
    frames.clear();
    nextSequence = 0;
    initialized = false;
    targetDelay = 3;
    prebuffering = true;
    consecutiveMisses = 0;
    consecutiveHits = 0;
}

} // namespace AV
} // namespace Core
} // namespace Acheron
