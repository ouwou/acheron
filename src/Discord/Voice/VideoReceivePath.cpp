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

namespace {

struct AnnouncedVideo
{
    QList<VideoLayer> layers;
    int maxWidth = 0;
    int maxHeight = 0;
    int maxFramerate = 0;
};

AnnouncedVideo announcedVideo(const ClientConnectData &data, bool streamContext)
{
    AnnouncedVideo announced;
    if (data.videoSsrc.get() == 0)
        return announced;

    QList<VideoLayer> flaggedInactive;
    for (const StreamDescription &stream : data.streams.valueOr()) {
        if (!stream.ssrc.hasValue() || stream.ssrc.get() == 0)
            continue;
        VideoLayer layer;
        layer.ssrc = stream.ssrc.get();
        layer.rtxSsrc = stream.rtxSsrcOrNext();
        layer.quality = stream.quality.valueOr(FULL_QUALITY);
        if (!stream.active.valueOr(true)) {
            flaggedInactive.append(layer);
            continue;
        }
        announced.layers.append(layer);

        if (layer.quality == FULL_QUALITY || announced.maxWidth == 0) {
            if (stream.maxResolution.hasValue()) {
                announced.maxWidth = stream.maxResolution->width;
                announced.maxHeight = stream.maxResolution->height;
            }
            announced.maxFramerate = stream.maxFramerate.valueOr(0);
        }
    }
    // on a voice connection the official client goes by the video SSRC alone and ignores each entry's
    // active flag
    if (announced.layers.isEmpty() && !streamContext)
        announced.layers = flaggedInactive;
    if (announced.layers.isEmpty() && data.streams.valueOr().isEmpty()) {
        VideoLayer layer;
        layer.ssrc = data.videoSsrc;
        layer.rtxSsrc = layer.ssrc + 1;
        announced.layers.append(layer);
    }
    return announced;
}

} // namespace

VoiceClient::VideoReceivePath::VideoReceivePath(VoiceClient &client) : client(client), tick(std::make_unique<QTimer>(&client))
{
    tick->setTimerType(Qt::PreciseTimer);
    QObject::connect(tick.get(), &QTimer::timeout, &client, [this] { onTick(); });
    for (const Codec &codec : client.params.videoCodecs) {
        const VideoCodec known = videoCodecFromName(codec.name);
        if (known == VideoCodec::Unknown || !codec.decode.valueOr(false))
            continue;
        payloadTypes.insert(codec.payloadType, known);
        if (codec.rtxPayloadType.hasValue())
            rtxPayloadTypes.insert(codec.rtxPayloadType.get(), known);
    }
}

VoiceClient::VideoReceivePath::~VideoReceivePath() = default;

void VoiceClient::VideoReceivePath::reset()
{
    tick->stop();
    state = {};
}

bool VoiceClient::VideoReceivePath::receive(const ParsedRtp &rtp, PacketPool::Handle &packet)
{
    const int payloadType = rtp.header.payloadType;
    if (const auto video = payloadTypes.constFind(payloadType); video != payloadTypes.constEnd()) {
        receiveVideo(rtp, std::move(packet), video.value(), VideoReceiver::Arrival::FirstTime);
        return true;
    }
    if (const auto rtx = rtxPayloadTypes.constFind(payloadType); rtx != rtxPayloadTypes.constEnd()) {
        receiveVideo(rtp, std::move(packet), rtx.value(), VideoReceiver::Arrival::Retransmitted);
        return true;
    }
    return false;
}

void VoiceClient::VideoReceivePath::noteTransportSequence(std::span<const uint8_t> extensions)
{
    if (!tick->isActive())
        return;
    const std::span<const uint8_t> sequence = findRtpExtension(extensions, TRANSPORT_SEQUENCE_EXTENSION);
    if (sequence.size() == 2)
        state.transportFeedback.onPacket(qFromBigEndian<uint16_t>(sequence.data()), nowUs());
}

void VoiceClient::VideoReceivePath::receiveVideo(const ParsedRtp &rtp, PacketPool::Handle packet, VideoCodec codec, VideoReceiver::Arrival arrival)
{
    const RtpHeader &header = rtp.header;
    if (!client.openRtpPayload(rtp, *packet))
        return;
    // the transport numbering is the connection's, not any one stream's
    state.transportMediaSsrc = header.ssrc;

    const bool retransmission = arrival == VideoReceiver::Arrival::Retransmitted;
    quint32 ssrc = header.ssrc;
    uint16_t sequence = header.sequence;
    if (retransmission) {
        // RFC 4588. one with no payload is probe padding
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
        if (retransmission)
            return;
        video.receiver = std::make_unique<VideoReceiver>(ssrc, codec);
    }

    const qint64 nowMs = clock.elapsed();
    if (packet->size() == 0) {
        // padding on the media stream itself still takes its sequence number
        video.receiver->skip(sequence, header.timestamp, nowMs);
        return;
    }
    video.receiver->insert(sequence, header.timestamp, header.marker, std::move(packet), nowMs, arrival);
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
                    state.decryptFailures++;
                    qCDebug(LogDave) << "Undecryptable" << describeFrame(codec, frame.data);
                    video.receiver->requestKeyframe();
                    return;
                }
            } else if (!dave->isDowngraded()) {
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
    decrypted.resize(client.daveSession->maxPlaintextSize(discord::dave::MediaType::Video, ssrc, frame.size()));
    const std::optional<size_t> written = client.daveSession->decrypt(discord::dave::MediaType::Video, ssrc, frame, decrypted);
    if (!written)
        return false;
    decrypted.resize(*written);
    frame.swap(decrypted);
    return true;
}

void VoiceClient::VideoReceivePath::onClientConnect(const ClientConnectData &data)
{
    if (payloadTypes.isEmpty() || !data.userId.hasValue())
        return;
    const Core::Snowflake userId = data.userId.get();
    if (!userId.isValid() || userId == client.params.userId)
        return;

    const AnnouncedVideo announced = announcedVideo(data, client.params.streamContext);
    const QList<VideoLayer> &layers = announced.layers;

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

    if (!state.explicitPlan && client.params.streamContext)
        planSingleSenders();
    emit client.remoteVideoLayersChanged(userId, layers);
    emit client.remoteVideoChanged(userId, !layers.isEmpty(), announced.maxWidth, announced.maxHeight, announced.maxFramerate);
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

// as the official client, which rebuilds every video receiver it has for the sender
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
    if (const std::optional<InboundEstimate> &estimate = received.inboundEstimate) {
        if (!state.serverEstimate || estimate->bps != state.serverEstimate->bps)
            qCDebug(LogVoice) << "Server's estimate of the way here:" << estimate->bps << "bps, loss" << estimate->lossFraction;
        state.serverEstimate = estimate;
    }
}

void VoiceClient::VideoReceivePath::onConnected()
{
    if (!payloadTypes.isEmpty()) {
        if (!clock.isValid())
            clock.start();
        state.reportedMs = clock.elapsed();

        tick->start(TICK_MS);
    }
    sendWants();
}

void VoiceClient::VideoReceivePath::onTick()
{
    if (!client.canSendMedia())
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

    // a compound packet must open with a report. NACKs and PLIs go first; transport feedback takes the
    // room left
    PacketPool::Handle packet = client.udpTransport->acquirePacket();
    packet->resize(packet->tailroom() - VoiceEncryption::TRAILER_SIZE);
    int size = writeReceiverReport(packet->bytes(), client.localSsrc, reports);
    if (size < 0)
        return;
    const auto room = [&packet, &size] { return packet->bytes().subspan(static_cast<size_t>(size)); };
    const auto added = [&size](int written) {
        if (written > 0)
            size += written;
        return written > 0;
    };
    for (const Pending &entry : pending) {
        if (!entry.feedback.nacks.empty())
            added(writeNack(room(), client.localSsrc, entry.ssrc, entry.feedback.nacks));
        if (entry.feedback.keyframeRequest)
            added(writePli(room(), client.localSsrc, entry.ssrc));
    }
    if (transportDue) {
        state.transportReportedMs = now;
        for (int i = 0; i < MAX_TRANSPORT_FEEDBACKS_PER_PACKET && state.transportFeedback.hasPending(); i++) {
            if (!added(state.transportFeedback.write(room(), client.localSsrc, state.transportMediaSsrc)))
                break;
        }
    }
    packet->resize(size);

    if (client.encryption->seal(*packet, RTCP_CLEAR_SIZE))
        client.udpTransport->send(*packet);

    if (reportDue) {
        state.reportedMs = now;
        stats.decryptFailures = state.decryptFailures;
        stats.serverEstimate = state.serverEstimate;
        if (!state.remoteVideos.empty())
            emit client.videoStatsUpdated(stats);
    }
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
