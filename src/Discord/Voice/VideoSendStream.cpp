#include "VideoSendStream.hpp"

#include <QtEndian>

#include <algorithm>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

constexpr qint64 HISTORY_US = qint64(VideoSendStream::HISTORY_MS) * 1000;
constexpr qint64 BURST_US = qint64(VideoSendStream::BURST_MS) * 1000;
constexpr qint64 MAX_QUEUE_US = qint64(VideoSendStream::MAX_QUEUE_MS) * 1000;
constexpr qint64 RETRANSMISSION_WINDOW_US = qint64(VideoSendStream::RETRANSMISSION_WINDOW_MS) * 1000;
constexpr size_t REMEMBERED_REPORTS = 8;
constexpr qint64 LEAST_DRAIN_US = 1000;

} // namespace

VideoSendStream::VideoSendStream(uint32_t ssrc, uint32_t rtxSsrc, const Numbering &first)
    : mediaSsrc(ssrc), retransmissionSsrc(rtxSsrc), nextSequence(first.sequence), nextRtxSequence(first.rtxSequence), nextTransportSequence(first.transportSequence), firstPictureId(first.pictureId)
{
}

void VideoSendStream::setCodec(VideoCodec codec, uint8_t mediaPayloadType, std::optional<uint8_t> retransmissionPayloadType)
{
    if (this->codec() == codec && payloadType == mediaPayloadType && rtxPayloadType == retransmissionPayloadType)
        return;

    clear();
    payloadType = mediaPayloadType;
    rtxPayloadType = retransmissionPayloadType;
    packetizer.reset();
    if (VideoPacketizer::supports(codec))
        packetizer.emplace(codec, firstPictureId);
}

void VideoSendStream::setBitrate(int bitsPerSecond)
{
    if (bitsPerSecond > 0)
        targetBitrate = bitsPerSecond;
}

void VideoSendStream::setPacingFactor(double factor)
{
    pacingFactor = factor;
}

int VideoSendStream::maxPayloadSize() const
{
    // as WebRTC, every packet leaves room to be resent whole
    return MAX_PACKET_SIZE - CLEAR_SIZE - EXTENSIONS_SIZE - (retransmits() ? RTX_HEADER_SIZE : 0);
}

bool VideoSendStream::queueFrame(std::span<const uint8_t> frame, uint32_t rtpTimestamp, bool keyframe, qint64 nowUs)
{
    if (!packetizer)
        return false;

    std::vector<std::vector<uint8_t>> payloads = packetizer->packetize(frame, keyframe, maxPayloadSize());
    if (payloads.empty())
        return false;

    for (size_t i = 0; i < payloads.size(); i++) {
        Packet &packet = queued.emplace_back();
        packet.payload = std::move(payloads[i]);
        packet.timestamp = rtpTimestamp;
        packet.marker = i + 1 == payloads.size();
        packet.queuedUs = nowUs;
        queuedBytes += static_cast<qint64>(packet.payload.size());
    }

    lastFrame = QueuedFrame{ rtpTimestamp, nowUs };
    counters.frames++;
    if (keyframe)
        counters.keyframes++;
    return true;
}

void VideoSendStream::retransmit(std::span<const uint16_t> sequences, qint64 nowUs)
{
    counters.nacksReceived += sequences.size();
    if (!retransmits())
        return;

    forgetOld(nowUs);
    for (uint16_t sequence : sequences) {
        Packet *sent = findSent(sequence);
        if (!sent || sent->resendWaiting)
            continue;
        if (sent->resentUs && nowUs - *sent->resentUs < roundTripUs())
            continue;
        if (!takeResendShare(static_cast<qint64>(sent->payload.size()), nowUs))
            break;
        sent->resendWaiting = true;
        resends.push_back(sequence);
    }
}

bool VideoSendStream::takeResendShare(qint64 bytes, qint64 nowUs)
{
    while (!resentLately.empty() && nowUs - resentLately.front().atUs >= RETRANSMISSION_WINDOW_US) {
        resentLatelyBytes -= resentLately.front().bytes;
        resentLately.pop_front();
    }

    const qint64 share = bytesAtRate(targetBitrate, RETRANSMISSION_WINDOW_US);
    if (resentLatelyBytes + bytes > share)
        return false;
    resentLately.push_back({ nowUs, bytes });
    resentLatelyBytes += bytes;
    return true;
}

void VideoSendStream::clear()
{
    queued.clear();
    resends.clear();
    history.clear();
    queuedBytes = 0;
    budget = 0;
    budgetedUs.reset();
}

qint64 VideoSendStream::pacingRate(qint64 nowUs) const
{
    const qint64 rate = static_cast<qint64>(targetBitrate * pacingFactor);
    if (queued.empty())
        return rate;
    const qint64 leftUs = std::max<qint64>(LEAST_DRAIN_US, MAX_QUEUE_US - (nowUs - queued.front().queuedUs));
    return std::max(rate, rateOf(queuedBytes, leftUs));
}

void VideoSendStream::startInterval(qint64 nowUs)
{
    const qint64 rate = pacingRate(nowUs);
    const qint64 burst = std::min<qint64>(bytesAtRate(rate, BURST_US), MAX_BURST_BYTES);
    const qint64 earnedUs = budgetedUs ? std::clamp<qint64>(nowUs - *budgetedUs, 0, BURST_US) : BURST_US;
    budgetedUs = nowUs;
    budget = std::min(burst, budget + bytesAtRate(rate, earnedUs));
}

bool VideoSendStream::writeNext(qint64 nowUs, PacketBuffer &out)
{
    if (budget <= 0)
        return false;
    forgetOld(nowUs);

    while (!resends.empty()) {
        const uint16_t sequence = resends.front();
        resends.pop_front();

        Packet *original = findSent(sequence);
        if (!original)
            continue;
        original->resendWaiting = false;
        if (!write(out, *original, Transmission::Retransmission, nowUs))
            continue;
        original->resentUs = nowUs;
        budget -= out.size();
        counters.retransmitted++;
        return true;
    }

    while (!queued.empty()) {
        Packet packet = std::move(queued.front());
        queued.pop_front();
        queuedBytes -= static_cast<qint64>(packet.payload.size());
        packet.sequence = nextSequence;
        packet.sentUs = nowUs;
        if (!write(out, packet, Transmission::Original, nowUs))
            continue;

        nextSequence++;
        budget -= out.size();
        counters.packets++;
        counters.bytes += packet.payload.size();
        history.push_back(std::move(packet));
        return true;
    }
    return false;
}

bool VideoSendStream::write(PacketBuffer &out, const Packet &packet, Transmission transmission, qint64 nowUs)
{
    const bool retransmission = transmission == Transmission::Retransmission;
    RtpHeader header;
    header.extension = true;
    header.marker = packet.marker;
    header.payloadType = retransmission ? *rtxPayloadType : payloadType;
    header.sequence = retransmission ? nextRtxSequence : packet.sequence;
    header.timestamp = packet.timestamp;
    header.ssrc = retransmission ? retransmissionSsrc : mediaSsrc;

    // abs-send-time: seconds as 6.18 fixed point, low 24 bits
    const quint64 sendTime = ((static_cast<quint64>(nowUs) << 18) + 500000) / 1000000;
    uint8_t sendTimeBytes[3];
    writeBigEndian24(static_cast<uint32_t>(sendTime), sendTimeBytes);
    // always zero, as the official client sends it
    const uint8_t noOffset[3] = {};
    const int size = CLEAR_SIZE + EXTENSIONS_SIZE + (retransmission ? RTX_HEADER_SIZE : 0) + static_cast<int>(packet.payload.size());
    uint8_t transportSequence[2];
    qToBigEndian(transport ? transport->numberPacket(nowUs, size) : nextTransportSequence, transportSequence);
    // WebRTC's order, not the ids' order
    const RtpExtension elements[] = { { ABS_SEND_TIME_EXTENSION, sendTimeBytes }, { TRANSMISSION_OFFSET_EXTENSION, noOffset }, { TRANSPORT_SEQUENCE_EXTENSION, transportSequence } };
    uint8_t extensions[EXTENSIONS_SIZE];
    if (writeRtpExtensions(elements, extensions) != EXTENSIONS_SIZE)
        return false;

    out.clear();
    if (writeRtpHeader(header, out.writable(), EXTENSIONS_SIZE / 4) != CLEAR_SIZE)
        return false;
    out.resize(CLEAR_SIZE);
    out.append(extensions);
    if (retransmission) {
        uint8_t originalSequence[RTX_HEADER_SIZE];
        qToBigEndian(packet.sequence, originalSequence);
        out.append(originalSequence);
    }
    if (!out.append(packet.payload))
        return false;

    nextTransportSequence++;
    if (retransmission)
        nextRtxSequence++;
    counters.bytesWritten += static_cast<quint64>(out.size());
    return true;
}

VideoSendStream::Packet *VideoSendStream::findSent(uint16_t sequence)
{
    if (history.empty())
        return nullptr;
    const uint16_t index = static_cast<uint16_t>(sequence - history.front().sequence);
    return index < history.size() ? &history[index] : nullptr;
}

qint64 VideoSendStream::roundTripUs() const
{
    return counters.roundTripMs ? qint64(*counters.roundTripMs + ROUND_TRIP_MARGIN_MS) * 1000 : 0;
}

void VideoSendStream::forgetOld(qint64 nowUs)
{
    const qint64 keptUs = std::max(HISTORY_US, HISTORY_ROUND_TRIPS * roundTripUs());
    while (!history.empty()) {
        const Packet &oldest = history.front();
        if (history.size() < size_t(MAX_HISTORY_PACKETS)) {
            const qint64 age = nowUs - oldest.sentUs;
            if (oldest.resendWaiting || age <= keptUs)
                return;
            if (history.size() < size_t(HISTORY_PACKETS) && age <= keptUs * SPARSE_HISTORY_FACTOR)
                return;
        }
        history.pop_front();
    }
}

int VideoSendStream::writeSenderReport(std::span<uint8_t> out, qint64 nowUs, uint64_t ntpTime)
{
    if (!canReport())
        return -1;

    SenderInfo info;
    info.ssrc = mediaSsrc;
    info.ntpTime = ntpTime;
    info.rtpTimestamp = lastFrame->timestamp + static_cast<uint32_t>((nowUs - lastFrame->queuedUs) * VIDEO_CLOCK_KHZ / 1000);
    info.packetCount = static_cast<uint32_t>(counters.packets);
    info.octetCount = static_cast<uint32_t>(counters.bytes);

    const int size = Voice::writeSenderReport(out, info);
    if (size < 0)
        return size;

    sentReports.push_back({ compactNtp(ntpTime), nowUs });
    if (sentReports.size() > REMEMBERED_REPORTS)
        sentReports.pop_front();
    return size;
}

void VideoSendStream::onReceptionReport(const ReceptionReport &report, qint64 nowUs)
{
    counters.lossFraction = report.fractionLost / 256.0;

    // zero: no sender report received yet
    if (report.lastSenderReport == 0)
        return;
    const auto answered = std::find_if(sentReports.begin(), sentReports.end(), [&report](const SentReport &sent) { return sent.compactNtp == report.lastSenderReport; });
    if (answered == sentReports.end())
        return;
    const qint64 heldUs = fromCompactNtpDelay(report.delaySinceLastSenderReport);
    counters.roundTripMs = static_cast<int>(std::max<qint64>(0, nowUs - answered->sentUs - heldUs) / 1000);
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
