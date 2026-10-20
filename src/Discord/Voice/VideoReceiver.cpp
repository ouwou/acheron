#include "VideoReceiver.hpp"

#include <algorithm>
#include <cmath>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

constexpr int VIDEO_CLOCK_KHZ = 90;

// Unwrapped sequence numbers start well above zero so that a packet from just before the first
// one seen never unwraps to a negative number.
constexpr qint64 SEQUENCE_BASE = qint64(1) << 20;

} // namespace

VideoReceiver::VideoReceiver(uint32_t ssrc, VideoCodec codec)
    : mediaSsrc(ssrc), videoCodec(codec)
{
}

qint64 VideoReceiver::unwrap(uint16_t sequence) const
{
    if (highestSequence < 0)
        return SEQUENCE_BASE + sequence;

    // The 16-bit number nearest to the highest one seen so far.
    const qint64 delta = static_cast<int16_t>(static_cast<uint16_t>(sequence - static_cast<uint16_t>(highestSequence)));
    return highestSequence + delta;
}

void VideoReceiver::noteArrival(uint32_t timestamp, qint64 nowMs)
{
    const qint64 transit = nowMs * VIDEO_CLOCK_KHZ - static_cast<qint64>(timestamp);
    if (haveTransit) {
        // The RTP timestamp wraps, so only the low 32 bits of the difference mean anything.
        const double difference = std::abs(static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(transit - lastTransit))));
        jitter += (difference - jitter) / 16.0;
    }
    lastTransit = transit;
    haveTransit = true;
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

    qint64 unwrapped = unwrap(sequence);
    counters.packets++;

    const bool farFromExpected = highestSequence >= 0 && std::abs(unwrapped - highestSequence) > MAX_SEQUENCE_JUMP;
    const bool continuesNewNumbering = farFromExpected && sequenceAfterFarPacket == sequence;
    sequenceAfterFarPacket = farFromExpected && !continuesNewNumbering ? std::optional<uint16_t>(static_cast<uint16_t>(sequence + 1)) : std::nullopt;
    if (farFromExpected && !continuesNewNumbering) {
        // RFC 3550, A.1: one such packet is a stray, a long-delayed duplicate say. Only a
        // second one following on from it shows that the sender restarted its numbering.
        counters.strays++;
        return;
    }

    if (continuesNewNumbering) {
        // Nothing held relates to what comes next.
        resync();
        highestSequence = -1;
        baseSequence = -1;
        received = 0;
        expectedAtLastReport = 0;
        receivedAtLastReport = 0;
        unwrapped = unwrap(sequence);
    }

    if (held.count(unwrapped) || (nextSequence >= 0 && unwrapped < nextSequence)) {
        counters.duplicates++;
        return;
    }

    if (baseSequence < 0)
        baseSequence = unwrapped;
    received++;

    if (unwrapped > highestSequence) {
        if (highestSequence >= 0 && !keyframeWanted) {
            for (qint64 gap = highestSequence + 1; gap < unwrapped; gap++)
                missing.emplace(gap, Missing{ nowMs, 0, 0 });
        }
        highestSequence = unwrapped;
        if (!retransmission)
            noteArrival(timestamp, nowMs);
    } else if (missing.erase(unwrapped) && retransmission) {
        counters.retransmitted++;
    }

    // The frame begins at its first packet, which may come before this one: senders put
    // delimiters and the like ahead of the parameter sets.
    qint64 frameStart = unwrapped;
    for (auto earlier = held.find(frameStart - 1); earlier != held.end() && earlier->second.payload && earlier->second.timestamp == timestamp; earlier = held.find(frameStart - 1))
        frameStart--;
    // A packet that starts the picture but brings no parameter sets is only taken as the
    // start when the packet before it is here to show that nothing of the frame was missed.
    // Right after subscribing, the parameter sets may have gone by already.
    const bool startKnown = payload && (startsWithParameterSets(videoCodec, payload->bytes()) || held.count(frameStart - 1));

    if (keyframeWanted && startKnown && startsKeyframe(videoCodec, payload->bytes())) {
        keyframeWanted = false;
        nextSequence = frameStart;
        held.erase(held.begin(), held.lower_bound(nextSequence));
        // Losses were not tracked while waiting, so note what is absent behind this packet.
        missing.clear();
        for (qint64 later = unwrapped + 1; later < highestSequence; later++) {
            if (!held.count(later))
                missing.emplace(later, Missing{ nowMs, 0, 0 });
        }
    }

    if (keyframeWanted && nextSequence < 0) {
        // Nothing before a key frame is of use, but packets of the key frame itself may arrive
        // ahead of its first one, so a short tail is kept.
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

    assemble();
}

void VideoReceiver::assemble()
{
    std::vector<std::span<const uint8_t>> payloads;

    while (nextSequence >= 0) {
        auto first = held.find(nextSequence);
        if (first == held.end())
            return;
        if (!first->second.payload) {
            held.erase(first);
            nextSequence++;
            continue;
        }

        // A frame is every packet sharing a timestamp; its last one carries the marker.
        payloads.clear();
        auto last = first;
        bool complete = false;
        for (auto it = first; it != held.end(); ++it) {
            if (it->first != nextSequence + static_cast<qint64>(payloads.size()))
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
    nextSequence = -1;
    // When the request goes out is left to poll(), which spaces requests apart however often
    // the stream has to start over.
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

    const bool silent = lastPacketMs < 0 || nowMs - lastPacketMs >= SILENT_AFTER_MS;
    if (keyframeWanted && !silent && (keyframeRequestedMs < 0 || nowMs - keyframeRequestedMs >= KEYFRAME_REQUEST_INTERVAL_MS)) {
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
    if (baseSequence < 0)
        return report;

    const qint64 expected = highestSequence - baseSequence + 1;
    const qint64 lost = expected - static_cast<qint64>(received);
    const qint64 expectedNow = expected - expectedAtLastReport;
    const qint64 receivedNow = static_cast<qint64>(received - receivedAtLastReport);
    const qint64 lostNow = expectedNow - receivedNow;
    expectedAtLastReport = expected;
    receivedAtLastReport = received;

    report.cumulativeLost = static_cast<int32_t>(std::clamp<qint64>(lost, -0x800000, 0x7FFFFF));
    if (expectedNow > 0 && lostNow > 0)
        report.fractionLost = static_cast<uint8_t>(std::min<qint64>(255, (lostNow << 8) / expectedNow));
    report.highestSequence = static_cast<uint32_t>(highestSequence - SEQUENCE_BASE);
    report.jitter = static_cast<uint32_t>(jitter);
    return report;
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
