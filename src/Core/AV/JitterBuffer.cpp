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
}

QByteArray JitterBuffer::pop()
{
    if (!initialized)
        return {};

    QByteArray data = frames.take(nextSequence);
    nextSequence++;
    return data;
}

void JitterBuffer::reset()
{
    frames.clear();
    nextSequence = 0;
    initialized = false;
}

} // namespace AV
} // namespace Core
} // namespace Acheron
