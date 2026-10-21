#include "VideoReceiver.hpp"

#include "RtpPacket.hpp"

#include <algorithm>
#include <cmath>

namespace Acheron {
namespace Discord {
namespace Voice {

VideoReceiver::VideoReceiver(uint32_t ssrc, VideoCodec codec)
    : mediaSsrc(ssrc), videoCodec(codec)
{
}

void VideoReceiver::noteArrival(uint32_t timestamp, qint64 nowMs)
{
    const qint64 transit = nowMs * VIDEO_CLOCK_KHZ - static_cast<qint64>(timestamp);
    if (lastTransit) {
        const double difference = std::abs(static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(transit - *lastTransit))));
        jitter += (difference - jitter) / 16.0;
    }
    lastTransit = transit;
}

void VideoReceiver::insert(uint16_t sequence, uint32_t timestamp, bool marker, PacketPool::Handle payload, qint64 nowMs, Arrival arrival)
{
    if (!payload || payload->size() == 0)
        return;
    counters.bytes += static_cast<quint64>(payload->size());
    take(sequence, timestamp, marker, std::move(payload), nowMs, arrival);
}

void VideoReceiver::skip(uint16_t sequence, uint32_t timestamp, qint64 nowMs)
{
    take(sequence, timestamp, false, {}, nowMs, Arrival::FirstTime);
}

void VideoReceiver::take(uint16_t sequence, uint32_t timestamp, bool marker, PacketPool::Handle payload, qint64 nowMs, Arrival arrival)
{
    const bool retransmission = arrival == Arrival::Retransmitted;
    lastPacketMs = nowMs;

    qint64 unwrapped = sequences.unwrap(sequence);
    counters.packets++;

    const std::optional<qint64> highest = sequences.highestSeen();
    const bool farFromExpected = highest && std::abs(unwrapped - *highest) > MAX_SEQUENCE_JUMP;
    const bool continuesNewNumbering = farFromExpected && sequenceAfterFarPacket == sequence;
    sequenceAfterFarPacket = farFromExpected && !continuesNewNumbering ? std::optional<uint16_t>(static_cast<uint16_t>(sequence + 1)) : std::nullopt;
    if (farFromExpected && !continuesNewNumbering) {
        // RFC 3550, A.1
        counters.strays++;
        return;
    }

    if (continuesNewNumbering) {
        resync();
        sequences.reset();
        reception = {};
        unwrapped = sequences.unwrap(sequence);
    }

    if (held.count(unwrapped) || (nextSequence && unwrapped < *nextSequence))
        return;

    if (!reception.baseSequence)
        reception.baseSequence = unwrapped;
    reception.received++;

    const std::optional<qint64> highestBefore = sequences.highestSeen();
    const bool extendsRun = highestBefore && unwrapped == *highestBefore + 1;
    const auto predecessor = held.find(unwrapped - 1);
    const bool continuesFrame = payload && predecessor != held.end() && predecessor->second.payload && predecessor->second.timestamp == timestamp;
    if (!highestBefore || unwrapped > *highestBefore) {
        if (highestBefore && !keyframeWanted) {
            for (qint64 gap = *highestBefore + 1; gap < unwrapped; gap++)
                missing.emplace(gap, Missing{ nowMs, 0, 0 });
        }
        sequences.advance(unwrapped);
        if (!retransmission)
            noteArrival(timestamp, nowMs);
    } else if (missing.erase(unwrapped) && retransmission) {
        counters.retransmitted++;
    }

    bool keyframeFound = false;
    if (keyframeWanted && payload && startsKeyframe(videoCodec, payload->bytes())) {
        // senders put delimiters and the like before the parameter sets
        qint64 frameStart = unwrapped;
        for (auto earlier = held.find(frameStart - 1); earlier != held.end() && earlier->second.payload && earlier->second.timestamp == timestamp; earlier = held.find(frameStart - 1))
            frameStart--;
        // right after subscribing, the parameter sets may have gone by
        keyframeFound = startsWithParameterSets(videoCodec, payload->bytes()) || held.count(frameStart - 1);
        if (keyframeFound)
            nextSequence = frameStart;
    }

    if (keyframeFound) {
        keyframeWanted = false;
        held.erase(held.begin(), held.lower_bound(*nextSequence));
        missing.clear();
        for (qint64 later = unwrapped + 1; later < *sequences.highestSeen(); later++) {
            if (!held.count(later))
                missing.emplace(later, Missing{ nowMs, 0, 0 });
        }
    }

    if (keyframeWanted && !nextSequence) {
        static constexpr int TAIL = 64;
        held.erase(held.begin(), held.lower_bound(unwrapped - TAIL));
    }

    Held entry;
    entry.payload = std::move(payload);
    entry.timestamp = timestamp;
    entry.marker = marker;
    held.emplace(unwrapped, std::move(entry));

    if (static_cast<int>(held.size()) > MAX_HELD_PACKETS) {
        resync();
        return;
    }

    // a packet that only carries on the frame in front of it completes nothing, so a frame of N packets
    // is put together once instead of looked over N times
    if (marker || keyframeFound || !extendsRun || !continuesFrame)
        assemble();
}

void VideoReceiver::assemble()
{
    std::vector<std::span<const uint8_t>> payloads;

    while (nextSequence) {
        auto first = held.find(*nextSequence);
        if (first == held.end())
            return;
        if (!first->second.payload) {
            held.erase(first);
            ++*nextSequence;
            continue;
        }

        payloads.clear();
        auto last = first;
        bool complete = false;
        for (auto it = first; it != held.end(); ++it) {
            if (it->first != *nextSequence + static_cast<qint64>(payloads.size()))
                return;
            if (!it->second.payload || it->second.timestamp != first->second.timestamp) {
                complete = true;
                break;
            }
            payloads.push_back(it->second.payload->bytes());
            last = it;
            if (it->second.marker) {
                complete = true;
                break;
            }
        }
        if (!complete)
            return;

        EncodedVideoFrame frame;
        frame.rtpTimestamp = first->second.timestamp;
        frame.keyframe = std::any_of(payloads.begin(), payloads.end(), [this](std::span<const uint8_t> payload) { return startsKeyframe(videoCodec, payload); });
        const bool assembled = assembleFrame(videoCodec, payloads, frame.data);

        nextSequence = last->first + 1;
        held.erase(first, std::next(last));

        if (!assembled) {
            resync();
            return;
        }

        counters.frames++;
        if (frame.keyframe)
            counters.keyframes++;
        ready.push_back(std::move(frame));
    }
}

void VideoReceiver::resync()
{
    held.clear();
    missing.clear();
    nextSequence.reset();
    keyframeWanted = true;
    counters.resyncs++;
}

void VideoReceiver::requestKeyframe()
{
    if (!keyframeWanted)
        resync();
}

std::vector<EncodedVideoFrame> VideoReceiver::takeFrames()
{
    std::vector<EncodedVideoFrame> frames;
    frames.swap(ready);
    return frames;
}

VideoReceiver::Feedback VideoReceiver::poll(qint64 nowMs)
{
    Feedback feedback;

    for (auto it = missing.begin(); it != missing.end(); ++it) {
        Missing &entry = it->second;
        if (entry.requests >= NACK_MAX_REQUESTS || nowMs - entry.noticedMs > NACK_GIVE_UP_MS) {
            resync();
            break;
        }
        if (nowMs - entry.noticedMs < REORDER_GRACE_MS)
            continue;
        if (entry.requests > 0 && nowMs - entry.requestedMs < NACK_RETRY_MS)
            continue;
        if (static_cast<int>(feedback.nacks.size()) >= MAX_NACKS_PER_POLL)
            break;

        entry.requests++;
        entry.requestedMs = nowMs;
        feedback.nacks.push_back(static_cast<uint16_t>(it->first));
    }
    if (keyframeWanted)
        feedback.nacks.clear();
    counters.nacked += feedback.nacks.size();

    const bool silent = !lastPacketMs || nowMs - *lastPacketMs >= SILENT_AFTER_MS;
    if (keyframeWanted && !silent && (!keyframeRequestedMs || nowMs - *keyframeRequestedMs >= KEYFRAME_REQUEST_INTERVAL_MS)) {
        keyframeRequestedMs = nowMs;
        feedback.keyframeRequest = true;
        counters.keyframeRequests++;
    }

    return feedback;
}

ReceptionReport VideoReceiver::receptionReport()
{
    ReceptionReport report;
    report.ssrc = mediaSsrc;
    if (!reception.baseSequence)
        return report;

    const qint64 highest = *sequences.highestSeen();
    const qint64 expected = highest - *reception.baseSequence + 1;
    const qint64 lost = expected - static_cast<qint64>(reception.received);
    const qint64 expectedNow = expected - reception.expectedAtLastReport;
    const qint64 receivedNow = static_cast<qint64>(reception.received - reception.receivedAtLastReport);
    const qint64 lostNow = expectedNow - receivedNow;
    reception.expectedAtLastReport = expected;
    reception.receivedAtLastReport = reception.received;

    report.cumulativeLost = static_cast<int32_t>(std::clamp<qint64>(lost, -0x800000, 0x7FFFFF));
    if (expectedNow > 0 && lostNow > 0)
        report.fractionLost = static_cast<uint8_t>(std::min<qint64>(255, (lostNow << 8) / expectedNow));
    report.highestSequence = static_cast<uint32_t>(highest - SequenceUnwrapper::BASE);
    report.jitter = static_cast<uint32_t>(jitter);
    return report;
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
