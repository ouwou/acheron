#include "TransportFeedback.hpp"

#include <QtEndian>

#include <cstdlib>
#include <vector>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

constexpr uint8_t RTCP_VERSION_BITS = 2 << 6;
constexpr uint8_t PADDING_BIT = 0x20;
constexpr uint8_t FORMAT_TRANSPORT_CC = 15;
constexpr uint8_t TYPE_TRANSPORT_FEEDBACK = 205;
constexpr int HEADER_SIZE = 20;

// Times are counted from a reference in units of 64 ms, and from packet to packet in units of
// 250 microseconds.
constexpr qint64 REFERENCE_UNIT_US = 64000;
constexpr qint64 DELTA_UNIT_US = 250;
constexpr qint64 SEQUENCE_BASE = qint64(1) << 20;

enum Status : uint8_t {
    NotReceived = 0,
    SmallDelta = 1,
    LargeDelta = 2,
};

constexpr int SYMBOLS_PER_VECTOR = 7;
constexpr int MAX_RUN_LENGTH = 0x1FFF;

// Chunks are either a run of one status or seven statuses spelled out two bits each.
void appendChunks(const std::vector<uint8_t> &statuses, std::vector<uint16_t> &chunks)
{
    size_t i = 0;
    while (i < statuses.size()) {
        size_t run = 1;
        while (i + run < statuses.size() && statuses[i + run] == statuses[i] && run < MAX_RUN_LENGTH)
            run++;

        if (run >= SYMBOLS_PER_VECTOR || i + run == statuses.size()) {
            chunks.push_back(static_cast<uint16_t>((statuses[i] << 13) | run));
            i += run;
            continue;
        }

        uint16_t vector = 0xC000;
        for (int slot = 0; slot < SYMBOLS_PER_VECTOR && i < statuses.size(); slot++, i++)
            vector |= static_cast<uint16_t>(statuses[i] << (2 * (SYMBOLS_PER_VECTOR - 1 - slot)));
        chunks.push_back(vector);
    }
}

} // namespace

qint64 TransportFeedback::unwrap(uint16_t sequence) const
{
    if (highestSequence < 0)
        return SEQUENCE_BASE + sequence;
    const qint64 delta = static_cast<int16_t>(static_cast<uint16_t>(sequence - static_cast<uint16_t>(highestSequence)));
    return highestSequence + delta;
}

void TransportFeedback::onPacket(uint16_t transportSequence, qint64 arrivalUs)
{
    qint64 sequence = unwrap(transportSequence);
    if (highestSequence >= 0 && std::llabs(sequence - highestSequence) > MAX_SEQUENCE_JUMP) {
        arrivals.clear();
        highestSequence = -1;
        reportedUpTo = -1;
        sequence = unwrap(transportSequence);
    }
    if (sequence < reportedUpTo)
        return;
    if (sequence > highestSequence)
        highestSequence = sequence;
    arrivals.emplace(sequence, arrivalUs);
}

int TransportFeedback::write(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc)
{
    if (arrivals.empty())
        return 0;

    const qint64 baseSequence = arrivals.begin()->first;
    const qint64 referenceTime = arrivals.begin()->second / REFERENCE_UNIT_US;

    // Walk forward from the first unreported packet, giving every sequence number up to the
    // last one that fits a status, and every packet that arrived a time.
    std::vector<uint8_t> statuses;
    std::vector<uint8_t> deltas;
    qint64 previousUs = referenceTime * REFERENCE_UNIT_US;
    qint64 sequence = baseSequence;
    auto next = arrivals.begin();
    while (next != arrivals.end() && static_cast<int>(statuses.size()) < MAX_STATUSES) {
        if (next->first != sequence) {
            statuses.push_back(NotReceived);
            sequence++;
            continue;
        }

        const qint64 delta = (next->second - previousUs) / DELTA_UNIT_US;
        // A gap in time too long to express ends this packet; the next one starts afresh.
        if (delta < INT16_MIN || delta > INT16_MAX)
            break;
        if (delta >= 0 && delta <= UINT8_MAX) {
            statuses.push_back(SmallDelta);
            deltas.push_back(static_cast<uint8_t>(delta));
        } else {
            statuses.push_back(LargeDelta);
            deltas.push_back(static_cast<uint8_t>(static_cast<uint16_t>(delta) >> 8));
            deltas.push_back(static_cast<uint8_t>(delta));
        }
        previousUs += delta * DELTA_UNIT_US;
        sequence++;
        ++next;
    }

    // Statuses after the last arrival say nothing.
    while (!statuses.empty() && statuses.back() == NotReceived)
        statuses.pop_back();
    if (statuses.empty()) {
        // Only possible when the very first packet is too far from its own reference, which
        // cannot happen; drop it rather than loop on it.
        arrivals.erase(arrivals.begin());
        return 0;
    }

    std::vector<uint16_t> chunks;
    appendChunks(statuses, chunks);

    const int unpadded = HEADER_SIZE + static_cast<int>(chunks.size()) * 2 + static_cast<int>(deltas.size());
    const int size = (unpadded + 3) & ~3;
    if (static_cast<int>(out.size()) < size)
        return -1;

    uint8_t *p = out.data();
    const int padding = size - unpadded;
    p[0] = static_cast<uint8_t>(RTCP_VERSION_BITS | (padding ? PADDING_BIT : 0) | FORMAT_TRANSPORT_CC);
    p[1] = TYPE_TRANSPORT_FEEDBACK;
    qToBigEndian(static_cast<uint16_t>(size / 4 - 1), p + 2);
    qToBigEndian(senderSsrc, p + 4);
    qToBigEndian(mediaSsrc, p + 8);
    qToBigEndian(static_cast<uint16_t>(baseSequence), p + 12);
    qToBigEndian(static_cast<uint16_t>(statuses.size()), p + 14);
    p[16] = static_cast<uint8_t>(referenceTime >> 16);
    p[17] = static_cast<uint8_t>(referenceTime >> 8);
    p[18] = static_cast<uint8_t>(referenceTime);
    p[19] = feedbackCount++;

    p += HEADER_SIZE;
    for (const uint16_t chunk : chunks) {
        qToBigEndian(chunk, p);
        p += 2;
    }
    for (const uint8_t delta : deltas)
        *p++ = delta;
    // Padding ends with its own length.
    for (int i = 0; i < padding; i++)
        *p++ = i + 1 == padding ? static_cast<uint8_t>(padding) : 0;

    arrivals.erase(arrivals.begin(), next);
    reportedUpTo = baseSequence + static_cast<qint64>(statuses.size());
    return size;
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
