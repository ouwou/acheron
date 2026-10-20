#include "VideoReceivePath.hpp"

#include "DaveSession.hpp"
#include "Rtcp.hpp"
#include "RtpPacket.hpp"
#include "UdpTransport.hpp"
#include "VoiceEncryption.hpp"
#include "VoiceGateway.hpp"

#include "Core/Logging.hpp"

#include <QTimer>
#include <QtEndian>

#include <algorithm>

namespace Acheron {
namespace Discord {
namespace Voice {

VoiceClient::VideoReceivePath::VideoReceivePath(VoiceClient &client) : client(client)
{
    for (const Codec &codec : client.params.videoCodecs) {
        const VideoCodec known = videoCodecFromName(codec.name);
        if (known == VideoCodec::Unknown || !codec.decode.valueOr(false))
            continue;
        payloadTypes.insert(codec.payloadType, known);
        if (codec.rtxPayloadType.hasValue())
            rtxPayloadTypes.insert(codec.rtxPayloadType.get(), known);
    }
}

VoiceClient::VideoReceivePath::~VideoReceivePath()
{
    delete tick;
}

void VoiceClient::VideoReceivePath::reset()
{
    delete std::exchange(tick, nullptr);
    state = {};
}

bool VoiceClient::VideoReceivePath::receive(const ParsedRtp &rtp, PacketPool::Handle &packet)
{
    const int payloadType = rtp.header.payloadType;
    if (const auto video = payloadTypes.constFind(payloadType); video != payloadTypes.constEnd()) {
        receiveVideo(rtp, std::move(packet), video.value(), false);
        return true;
    }
    if (const auto rtx = rtxPayloadTypes.constFind(payloadType); rtx != rtxPayloadTypes.constEnd()) {
        receiveVideo(rtp, std::move(packet), rtx.value(), true);
        return true;
    }
    return false;
}

void VoiceClient::VideoReceivePath::noteTransportSequence(std::span<const uint8_t> extensions)
{
    // Only while feedback is going out; otherwise the record would grow without end.
    if (!tick || !tick->isActive())
        return;
    const std::span<const uint8_t> sequence = findRtpExtension(extensions, TRANSPORT_SEQUENCE_EXTENSION);
    if (sequence.size() == 2)
        state.transportFeedback.onPacket(qFromBigEndian<uint16_t>(sequence.data()), nowUs());
}

void VoiceClient::VideoReceivePath::receiveVideo(const ParsedRtp &rtp, PacketPool::Handle packet, VideoCodec codec, bool retransmission)
{
    const RtpHeader &header = rtp.header;
    if (!client.encryption || !client.encryption->open(*packet, rtp.clearSize))
        return;

    packet->dropFront(rtp.clearSize);
    if (packet->size() < rtp.extensionSize)
        return;
    // Every packet the server numbered is reported back to it, wanted or not, since the
    // numbering is about the connection and not about any one stream.
    noteTransportSequence(packet->bytes().first(static_cast<size_t>(rtp.extensionSize)));
    state.transportMediaSsrc = header.ssrc;
    packet->dropFront(rtp.extensionSize);

    if (header.padding) {
        const int padding = rtpPaddingSize(packet->bytes());
        if (padding < 0)
            return;
        packet->resize(packet->size() - padding);
    }

    quint32 ssrc = header.ssrc;
    uint16_t sequence = header.sequence;
    if (retransmission) {
        // RFC 4588: the original sequence number leads the payload. One without a payload is
        // padding sent to probe the link.
        auto primary = state.rtxToPrimary.constFind(ssrc);
        if (primary == state.rtxToPrimary.constEnd() || packet->size() <= RTX_HEADER_SIZE)
            return;
        ssrc = primary.value();
        sequence = qFromBigEndian<uint16_t>(packet->data());
        packet->dropFront(RTX_HEADER_SIZE);
    }

    auto it = state.remoteVideos.find(ssrc);
    if (it == state.remoteVideos.end()) {
        client.unrouted.rtp++;
        return;
    }
    if (!state.deliveredSsrcs.contains(ssrc))
        return;

    RemoteVideo &video = it->second;
    if (!video.receiver || video.receiver->codec() != codec) {
        // A retransmission of what came before a change of codec repairs nothing now.
        if (retransmission)
            return;
        video.receiver = std::make_unique<VideoReceiver>(ssrc, codec);
    }

    const qint64 nowMs = clock.elapsed();
    if (packet->size() == 0) {
        // Padding a sender puts on the media stream itself still takes its sequence number.
        video.receiver->skip(sequence, header.timestamp, nowMs);
        return;
    }
    video.receiver->insert(sequence, header.timestamp, header.marker, std::move(packet), nowMs, retransmission ? VideoReceiver::Arrival::Retransmitted : VideoReceiver::Arrival::FirstTime);
    deliverFrames(ssrc, video);
}

void VoiceClient::VideoReceivePath::deliverFrames(quint32 ssrc, RemoteVideo &video)
{
    std::vector<EncodedVideoFrame> frames = video.receiver->takeFrames();
    const VideoCodec codec = video.receiver->codec();

    for (EncodedVideoFrame &frame : frames) {
        if (DaveSession *dave = client.daveSession.get()) {
            if (dave->isDaveEnabled()) {
                if (!daveDecrypt(ssrc, frame.data)) {
                    // Later frames build on this one, so they are of no use either.
                    state.decryptFailures++;
                    qCDebug(LogDave) << "Undecryptable" << describeFrame(codec, frame.data);
                    video.receiver->requestKeyframe();
                    return;
                }
            } else if (!dave->isDowngraded()) {
                // Keys are still being agreed. A key frame is asked for once they are.
                state.droppedAwaitingDaveKeys = true;
                return;
            }
        }

        prepareForDecoder(codec, frame.data);
        if (frameHandler)
            frameHandler(video.userId, codec, std::move(frame));
    }
}

bool VoiceClient::VideoReceivePath::daveDecrypt(quint32 ssrc, std::vector<uint8_t> &frame)
{
    const auto user = client.ssrcToUserIdMap.constFind(ssrc);
    auto *decryptor = client.daveSession->getOrCreateDecryptor(ssrc, user == client.ssrcToUserIdMap.constEnd() ? 0 : user.value());

    std::vector<uint8_t> plaintext(decryptor->GetMaxPlaintextByteSize(discord::dave::MediaType::Video, frame.size()));
    size_t bytesWritten = 0;
    const auto result = decryptor->Decrypt(discord::dave::MediaType::Video, discord::dave::ArrayView<const uint8_t>(frame.data(), frame.size()), discord::dave::ArrayView<uint8_t>(plaintext.data(), plaintext.size()), &bytesWritten);
    if (result != discord::dave::IDecryptor::Success) {
        qCDebug(LogDave) << "DAVE video decrypt failed: SSRC =" << ssrc << "result =" << static_cast<int>(result) << "frameSize =" << frame.size();
        return false;
    }

    plaintext.resize(bytesWritten);
    frame.swap(plaintext);
    return true;
}

void VoiceClient::VideoReceivePath::onClientConnect(const ClientConnectData &data)
{
    if (payloadTypes.isEmpty() || !data.userId.hasValue())
        return;
    const Core::Snowflake userId = data.userId.get();
    if (!userId.isValid() || userId == client.params.userId)
        return;

    // A user's video is on while the announcement names a video SSRC; each entry of `streams`
    // is then one encoding of it. An announcement with no entries means a single encoding.
    QList<VideoLayer> layers;
    QList<VideoLayer> flaggedInactive;
    int maxWidth = 0;
    int maxHeight = 0;
    int maxFramerate = 0;
    if (data.videoSsrc.get() != 0) {
        if (data.streams.hasValue()) {
            for (const StreamDescription &stream : data.streams.get()) {
                if (!stream.ssrc.hasValue() || stream.ssrc.get() == 0)
                    continue;
                VideoLayer layer;
                layer.ssrc = stream.ssrc.get();
                layer.rtxSsrc = stream.rtxSsrc.hasValue() && stream.rtxSsrc.get() != 0 ? stream.rtxSsrc.get() : layer.ssrc + 1;
                layer.quality = stream.quality.valueOr(FULL_QUALITY);
                if (!stream.active.valueOr(true)) {
                    flaggedInactive.append(layer);
                    continue;
                }
                layers.append(layer);

                if (layer.quality == FULL_QUALITY || maxWidth == 0) {
                    if (stream.maxResolution.hasValue()) {
                        maxWidth = stream.maxResolution->width;
                        maxHeight = stream.maxResolution->height;
                    }
                    maxFramerate = stream.maxFramerate.valueOr(0);
                }
            }
        }
        // On a voice connection the official client goes by the video SSRC alone and ignores
        // each entry's own flag, so entries all flagged inactive still count there.
        if (layers.isEmpty() && !client.params.streamContext)
            layers = flaggedInactive;
        if (layers.isEmpty() && (!data.streams.hasValue() || data.streams->isEmpty())) {
            VideoLayer layer;
            layer.ssrc = data.videoSsrc;
            layer.rtxSsrc = layer.ssrc + 1;
            layers.append(layer);
        }
    }

    const auto known = state.remoteLayers.constFind(userId);
    if (known == state.remoteLayers.constEnd() ? !layers.isEmpty() : known.value() != layers) {
        removeRemoteVideo(userId);
        for (const VideoLayer &layer : layers) {
            RemoteVideo video;
            video.userId = userId;
            video.rtxSsrc = layer.rtxSsrc;
            state.remoteVideos.emplace(layer.ssrc, std::move(video));
            state.rtxToPrimary.insert(layer.rtxSsrc, layer.ssrc);

            client.ssrcToUserIdMap.insert(layer.ssrc, userId);
            if (client.daveSession)
                client.daveSession->applyKeyRatchetForSsrc(layer.ssrc, userId);
        }
        if (!layers.isEmpty())
            state.remoteLayers.insert(userId, layers);
    }

    // On a voice connection whoever shows the video decides what is wanted, and is told here.
    if (!state.explicitPlan && client.params.streamContext)
        planSingleSenders();
    emit client.remoteVideoLayersChanged(userId, layers);
    emit client.remoteVideoChanged(userId, !layers.isEmpty(), maxWidth, maxHeight, maxFramerate);
}

void VoiceClient::VideoReceivePath::onClientDisconnect(Core::Snowflake userId)
{
    const bool hadVideo = state.remoteLayers.contains(userId);
    removeRemoteVideo(userId);
    if (!hadVideo)
        return;
    emit client.remoteVideoLayersChanged(userId, {});
    emit client.remoteVideoChanged(userId, false, 0, 0, 0);
}

void VoiceClient::VideoReceivePath::removeRemoteVideo(Core::Snowflake userId)
{
    for (auto it = state.remoteVideos.begin(); it != state.remoteVideos.end();) {
        if (it->second.userId != userId) {
            ++it;
            continue;
        }
        state.rtxToPrimary.remove(it->second.rtxSsrc);
        client.ssrcToUserIdMap.remove(it->first);
        state.deliveredSsrcs.remove(it->first);
        it = state.remoteVideos.erase(it);
    }
    state.remoteLayers.remove(userId);
}

void VoiceClient::VideoReceivePath::setWants(int pixelCount, bool wantedNow)
{
    state.explicitPlan = false;
    state.wantedPixelCount = pixelCount;
    state.wanted = wantedNow;
    planSingleSenders();
}

// The plan for a stream connection, where the only sender is the broadcaster: its first
// encoding at full quality, sized to the surface, or nothing while the surface is not shown.
void VoiceClient::VideoReceivePath::planSingleSenders()
{
    state.plannedWants = MediaSinkWants();
    state.deliveredSsrcs.clear();
    for (auto it = state.remoteLayers.constBegin(); it != state.remoteLayers.constEnd(); ++it) {
        const quint32 ssrc = it.value().first().ssrc;
        state.plannedWants.bySsrc.insert(ssrc, state.wanted ? FULL_QUALITY : 0);
        if (state.wanted) {
            state.plannedWants.pixelCounts.insert(ssrc, state.wantedPixelCount);
            state.deliveredSsrcs.insert(ssrc);
        }
    }
    applyPlan();
}

void VoiceClient::VideoReceivePath::setPlan(const MediaSinkWants &wants, const QSet<quint32> &deliver)
{
    state.explicitPlan = true;
    state.plannedWants = wants;
    state.deliveredSsrcs = deliver;
    applyPlan();
}

void VoiceClient::VideoReceivePath::applyPlan()
{
    // An encoding no longer taken starts from a key frame if it is taken again.
    for (auto &[ssrc, video] : state.remoteVideos) {
        if (!state.deliveredSsrcs.contains(ssrc))
            video.receiver.reset();
    }
    sendWants();
}

void VoiceClient::VideoReceivePath::sendWants()
{
    if (client.currentState != State::Connected || !client.gateway || payloadTypes.isEmpty())
        return;
    // A stream's viewer has nothing to say until the broadcaster's video is announced, and a
    // voice connection nothing until it has been given a plan.
    if (!state.explicitPlan && (!client.params.streamContext || state.remoteLayers.isEmpty()))
        return;

    const QByteArray serialized = state.plannedWants.toJson().toBytes();
    if (serialized == state.sentWants)
        return;
    state.sentWants = serialized;
    client.gateway->sendMediaSinkWants(state.plannedWants);
}

void VoiceClient::VideoReceivePath::requestKeyframeFrom(Core::Snowflake userId)
{
    for (auto &[ssrc, video] : state.remoteVideos) {
        if (video.receiver && video.userId == userId)
            video.receiver->requestKeyframe();
    }
}

void VoiceClient::VideoReceivePath::requestKeyframeFromEveryone()
{
    for (auto &[ssrc, video] : state.remoteVideos) {
        if (video.receiver)
            video.receiver->requestKeyframe();
    }
}

void VoiceClient::VideoReceivePath::onDaveStateChanged()
{
    if (std::exchange(state.droppedAwaitingDaveKeys, false))
        requestKeyframeFromEveryone();
}

// The official client tears down and rebuilds every video receiver it has for the sender, which
// leaves each waiting for a key frame and asking for one. Starting each over does the same.
void VoiceClient::VideoReceivePath::receiveAfresh(quint32 ssrc)
{
    Core::Snowflake sender;
    if (const auto known = client.ssrcToUserIdMap.constFind(ssrc); known != client.ssrcToUserIdMap.constEnd())
        sender = Core::Snowflake(known.value());
    for (const auto &[videoSsrc, video] : state.remoteVideos) {
        if (videoSsrc == ssrc || video.rtxSsrc == ssrc)
            sender = video.userId;
    }
    qCDebug(LogVoice) << "Server asks for the video of SSRC" << ssrc << "to be received afresh";
    if (sender.isValid())
        requestKeyframeFrom(sender);
}

void VoiceClient::VideoReceivePath::receiveRtcp(const ReceivedRtcp &received)
{
    for (uint32_t ssrc : received.videoResets)
        receiveAfresh(ssrc);
    if (clock.isValid()) {
        for (const ReceivedRtcp::SenderReport &report : received.senderReports)
            state.senderReports.insert(report.ssrc, { report.compactNtp, nowUs() });
    }
    if (received.hasInboundEstimate) {
        if (received.inboundBitsPerSecond != state.serverEstimateBps)
            qCDebug(LogVoice) << "Server's estimate of the way here:" << received.inboundBitsPerSecond << "bps, loss" << received.inboundLoss;
        state.serverEstimateBps = received.inboundBitsPerSecond;
        state.serverLossFraction = received.inboundLoss;
    }
}

void VoiceClient::VideoReceivePath::onConnected()
{
    if (!payloadTypes.isEmpty()) {
        if (!clock.isValid())
            clock.start();
        state.reportedMs = clock.elapsed();

        if (!tick) {
            tick = new QTimer(&client);
            tick->setTimerType(Qt::PreciseTimer);
            QObject::connect(tick, &QTimer::timeout, &client, [this] { onTick(); });
        }
        tick->start(TICK_MS);
    }
    sendWants();
}

void VoiceClient::VideoReceivePath::onTick()
{
    if (client.currentState != State::Connected || !client.encryption || !client.udpTransport)
        return;

    const qint64 now = clock.elapsed();
    const bool reportDue = now - state.reportedMs >= REPORT_INTERVAL_MS;

    struct Pending
    {
        quint32 ssrc;
        VideoReceiver::Feedback feedback;
    };
    std::vector<Pending> pending;
    std::vector<ReceptionReport> reports;
    VideoReceiveStats stats;

    for (auto &[ssrc, video] : state.remoteVideos) {
        if (!video.receiver)
            continue;

        VideoReceiver::Feedback feedback = video.receiver->poll(now);
        if (!feedback.nacks.empty() || feedback.keyframeRequest)
            pending.push_back({ ssrc, std::move(feedback) });

        if (!reportDue)
            continue;
        ReceptionReport report = video.receiver->receptionReport();
        if (const auto heard = state.senderReports.constFind(ssrc); heard != state.senderReports.constEnd()) {
            report.lastSenderReport = heard->compactNtp;
            report.delaySinceLastSenderReport = toCompactNtpDelay(nowUs() - heard->arrivalUs);
        }
        reports.push_back(report);
        const VideoReceiver::Stats &counters = video.receiver->stats();
        stats.codec = videoCodecName(video.receiver->codec());
        stats.packets += counters.packets;
        stats.bytes += counters.bytes;
        stats.frames += counters.frames;
        stats.keyframes += counters.keyframes;
        stats.retransmitted += counters.retransmitted;
        stats.nacked += counters.nacked;
        stats.keyframeRequests += counters.keyframeRequests;
        stats.lossFraction = std::max(stats.lossFraction, report.fractionLost / 256.0);
    }

    const bool transportDue = state.transportFeedback.hasPending() && now - state.transportReportedMs >= TRANSPORT_FEEDBACK_INTERVAL_MS;
    if (pending.empty() && reports.empty() && !transportDue) {
        if (reportDue)
            state.reportedMs = now;
        return;
    }

    // A compound packet opens with a report, so feedback sent between reports follows an empty
    // one. What the receivers have counted as asked for goes in first, ahead of the transport
    // feedback, which takes whatever room is left.
    PacketPool::Handle packet = client.udpTransport->acquirePacket();
    packet->resize(packet->tailroom() - VoiceEncryption::TRAILER_SIZE);
    int size = writeReceiverReport(packet->bytes(), client.localSsrc, reports);
    if (size < 0)
        return;
    for (const Pending &entry : pending) {
        if (!entry.feedback.nacks.empty()) {
            const int written = writeNack(packet->bytes().subspan(static_cast<size_t>(size)), client.localSsrc, entry.ssrc, entry.feedback.nacks);
            if (written > 0)
                size += written;
        }
        if (entry.feedback.keyframeRequest) {
            const int written = writePli(packet->bytes().subspan(static_cast<size_t>(size)), client.localSsrc, entry.ssrc);
            if (written > 0)
                size += written;
        }
    }
    if (transportDue) {
        state.transportReportedMs = now;
        // One packet covers a bounded span; after a long gap it takes more than one.
        for (int i = 0; i < 4 && state.transportFeedback.hasPending(); i++) {
            const int written = state.transportFeedback.write(packet->bytes().subspan(static_cast<size_t>(size)), client.localSsrc, state.transportMediaSsrc);
            if (written <= 0)
                break;
            size += written;
        }
    }
    packet->resize(size);

    if (client.encryption->seal(*packet, RTCP_CLEAR_SIZE))
        client.udpTransport->send(*packet);

    if (reportDue) {
        state.reportedMs = now;
        stats.decryptFailures = state.decryptFailures;
        stats.serverEstimateBps = state.serverEstimateBps;
        stats.serverLossFraction = state.serverLossFraction;
        if (!state.remoteVideos.empty())
            emit client.videoStatsUpdated(stats);
    }
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
