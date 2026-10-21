#include "TransportFeedback.hpp"

#include "Rtcp.hpp"

#include <QtEndian>

#include <cstdlib>
#include <vector>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

constexpr int HEADER_SIZE = 20;

constexpr int SYMBOLS_PER_VECTOR = 7;
constexpr int MAX_RUN_LENGTH = 0x1FFF;

void appendChunks(const std::vector<FeedbackStatus> &statuses, std::vector<uint16_t> &chunks)
{
    size_t i = 0;
    while (i < statuses.size()) {
        size_t run = 1;
        while (i + run < statuses.size() && statuses[i + run] == statuses[i] && run < MAX_RUN_LENGTH)
            run++;

        if (run >= SYMBOLS_PER_VECTOR || i + run == statuses.size()) {
            chunks.push_back(static_cast<uint16_t>((static_cast<uint8_t>(statuses[i]) << 13) | run));
            i += run;
            continue;
        }

        uint16_t vector = 0xC000;
        for (int slot = 0; slot < SYMBOLS_PER_VECTOR && i < statuses.size(); slot++, i++)
            vector |= static_cast<uint16_t>(static_cast<uint8_t>(statuses[i]) << (2 * (SYMBOLS_PER_VECTOR - 1 - slot)));
        chunks.push_back(vector);
    }
}

} // namespace

void TransportFeedback::onPacket(uint16_t transportSequence, qint64 arrivalUs)
{
    qint64 sequence = sequences.unwrap(transportSequence);
    if (const std::optional<qint64> highest = sequences.highestSeen(); highest && std::llabs(sequence - *highest) > MAX_SEQUENCE_JUMP) {
        arrivals.clear();
        sequences.reset();
        reportedUpTo.reset();
        sequence = sequences.unwrap(transportSequence);
    }
    if (reportedUpTo && sequence < *reportedUpTo)
        return;
    sequences.advance(sequence);
    arrivals.emplace(sequence, arrivalUs);
}

int TransportFeedback::write(std::span<uint8_t> out, uint32_t senderSsrc, uint32_t mediaSsrc)
{
    if (arrivals.empty())
        return 0;

    const qint64 baseSequence = arrivals.begin()->first;
    const qint64 referenceTime = arrivals.begin()->second / REFERENCE_UNIT_US;

    std::vector<FeedbackStatus> statuses;
    std::vector<uint8_t> deltas;
    qint64 previousUs = referenceTime * REFERENCE_UNIT_US;
    qint64 sequence = baseSequence;
    auto next = arrivals.begin();
    while (next != arrivals.end() && static_cast<int>(statuses.size()) < MAX_STATUSES) {
        if (next->first != sequence) {
            statuses.push_back(FeedbackStatus::NotReceived);
            sequence++;
            continue;
        }

        const qint64 delta = (next->second - previousUs) / DELTA_UNIT_US;
        if (delta < INT16_MIN || delta > INT16_MAX)
            break;
        if (delta >= 0 && delta <= UINT8_MAX) {
            statuses.push_back(FeedbackStatus::SmallDelta);
            deltas.push_back(static_cast<uint8_t>(delta));
        } else {
            statuses.push_back(FeedbackStatus::LargeDelta);
            deltas.push_back(static_cast<uint8_t>(static_cast<uint16_t>(delta) >> 8));
            deltas.push_back(static_cast<uint8_t>(delta));
        }
        previousUs += delta * DELTA_UNIT_US;
        sequence++;
        ++next;
    }

    while (!statuses.empty() && statuses.back() == FeedbackStatus::NotReceived)
        statuses.pop_back();
    if (statuses.empty()) {
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
    writeRtcpHeader(p, static_cast<uint8_t>((padding ? RTCP_PADDING_BIT : 0) | FORMAT_TRANSPORT_WIDE), TYPE_TRANSPORT_FEEDBACK, size);
    qToBigEndian(senderSsrc, p + 4);
    qToBigEndian(mediaSsrc, p + 8);
    qToBigEndian(static_cast<uint16_t>(baseSequence), p + 12);
    qToBigEndian(static_cast<uint16_t>(statuses.size()), p + 14);
    writeBigEndian24(static_cast<uint32_t>(referenceTime), p + 16);
    p[19] = feedbackCount++;

    p += HEADER_SIZE;
    for (const uint16_t chunk : chunks) {
        qToBigEndian(chunk, p);
        p += 2;
    }
    for (const uint8_t delta : deltas)
        *p++ = delta;
    // padding ends with its own length
    for (int i = 0; i < padding; i++)
        *p++ = i + 1 == padding ? static_cast<uint8_t>(padding) : 0;

    arrivals.erase(arrivals.begin(), next);
    reportedUpTo = baseSequence + static_cast<qint64>(statuses.size());
    return size;
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
