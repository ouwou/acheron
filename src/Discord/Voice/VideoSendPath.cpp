#include "VideoSendPath.hpp"

#include "DaveSession.hpp"
#include "Rtcp.hpp"
#include "UdpTransport.hpp"
#include "VoiceEncryption.hpp"
#include "VoiceGateway.hpp"

#include "Core/Logging.hpp"

#include <QDateTime>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

uint64_t ntpTimeNow()
{
    constexpr quint64 SECONDS_FROM_1900_TO_1970 = 2208988800ULL;
    const quint64 ms = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    return ((ms / 1000 + SECONDS_FROM_1900_TO_1970) << 32) | (((ms % 1000) << 32) / 1000);
}

std::optional<discord::dave::Codec> daveCodecOf(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264:
        return discord::dave::Codec::H264;
    case VideoCodec::H265:
        return discord::dave::Codec::H265;
    case VideoCodec::VP8:
        return discord::dave::Codec::VP8;
    case VideoCodec::AV1:
        return discord::dave::Codec::AV1;
    case VideoCodec::Unknown:
        break;
    }
    return std::nullopt;
}

template <typename T>
std::optional<T> worstOf(std::optional<T> a, std::optional<T> b)
{
    if (a && b)
        return std::max(*a, *b);
    return a ? a : b;
}

} // namespace

VoiceClient::VideoSendPath::VideoSendPath(VoiceClient &client)
    : client(client), paceTimer(std::make_unique<QTimer>(&client)), sendReportTimer(std::make_unique<QTimer>(&client)), keyframeTimer(std::make_unique<QTimer>(&client))
{
    clock.start();

    paceTimer->setTimerType(Qt::PreciseTimer);
    QObject::connect(paceTimer.get(), &QTimer::timeout, &client, [this] { onPaceTick(); });
    QObject::connect(sendReportTimer.get(), &QTimer::timeout, &client, [this] { onSendReportTick(); });
    keyframeTimer->setSingleShot(true);
    QObject::connect(keyframeTimer.get(), &QTimer::timeout, &client, [this] { askLayersForKeyframe([](const Layer &layer) { return layer.keyframeAskWaiting; }); });
}

VoiceClient::VideoSendPath::~VideoSendPath() = default;

void VoiceClient::VideoSendPath::reset()
{
    paceTimer->stop();
    sendReportTimer->stop();
    keyframeTimer->stop();
    for (Layer &layer : layers) {
        Layer fresh;
        fresh.quality = layer.quality;
        fresh.ssrc = layer.ssrc;
        fresh.rtxSsrc = layer.rtxSsrc;
        layer = std::move(fresh);
    }
    estimator = SendSideEstimator();
    reportedDelayEstimateBps.reset();
    started.reset();
    receiverEstimateBps.reset();
    announcedStreams.clear();
    keyframeRequests = 0;
}

void VoiceClient::VideoSendPath::takeStreams(const VoiceReady &ready)
{
    streams = ready.streams.valueOr();
    if (streams.isEmpty()) {
        // given none, the official client takes the two SSRCs after its audio one
        StreamDescription stream = cameraStreams(false).front();
        stream.ssrc = client.localSsrc + 1;
        stream.rtxSsrc = client.localSsrc + 2;
        streams.append(stream);
    }

    layers.clear();
    for (const StreamDescription &stream : std::as_const(streams)) {
        const quint32 ssrc = stream.ssrc.valueOr(0);
        if (ssrc == 0)
            continue;
        Layer layer;
        layer.quality = stream.quality.valueOr(FULL_QUALITY);
        layer.ssrc = ssrc;
        layer.rtxSsrc = stream.rtxSsrcOrNext();
        layers.push_back(std::move(layer));
    }
    emitLayersKnown();
}

void VoiceClient::VideoSendPath::emitLayersKnown()
{
    QList<int> qualities;
    for (const Layer &layer : layers)
        qualities.append(layer.quality);
    emit client.videoSendLayersKnown(qualities);
}

VoiceClient::VideoSendPath::Layer *VoiceClient::VideoSendPath::layerOfQuality(int quality)
{
    const auto found = std::find_if(layers.begin(), layers.end(), [quality](const Layer &layer) { return layer.quality == quality; });
    return found == layers.end() ? nullptr : &*found;
}

VoiceClient::VideoSendPath::Layer *VoiceClient::VideoSendPath::layerOf(quint32 ssrc)
{
    const auto found = std::find_if(layers.begin(), layers.end(), [ssrc](const Layer &layer) { return layer.ssrc == ssrc; });
    return found == layers.end() ? nullptr : &*found;
}

// the official client's capture limits apply to the full-quality layer, whose SSRC an announcement
// names at the top
const VoiceClient::VideoSendPath::Layer *VoiceClient::VideoSendPath::fullLayer() const
{
    const auto full = std::find_if(layers.cbegin(), layers.cend(), [](const Layer &layer) { return layer.quality == FULL_QUALITY; });
    if (full != layers.cend())
        return &*full;
    return layers.empty() ? nullptr : &layers.front();
}

quint32 VoiceClient::VideoSendPath::videoSsrc() const
{
    const Layer *full = fullLayer();
    return full ? full->ssrc : 0;
}

bool VoiceClient::VideoSendPath::anyWaiting() const
{
    return std::any_of(layers.cbegin(), layers.cend(), [](const Layer &layer) { return layer.sender && layer.sender->hasWaiting(); });
}

void VoiceClient::VideoSendPath::setServerCodec(const QString &name)
{
    serverCodecName = name;

    const VideoCodec named = videoCodecFromName(name);
    const VideoCodec codec = offeredForEncoding(named) ? named : VideoCodec::Unknown;
    if (codec == sendCodec)
        return;

    qCInfo(LogVoice) << "Video is to be sent as" << (codec == VideoCodec::Unknown ? QStringLiteral("nothing this client encodes") : name);
    sendCodec = codec;
    if (started) {
        prepareSenders();
        for (Layer &layer : layers)
            layer.awaitsKeyframe = true;
    }
    emit client.videoSendCodecChanged(codec);
}

const Codec *VoiceClient::VideoSendPath::offeredForEncoding(VideoCodec codec) const
{
    const QList<Codec> &offeredCodecs = client.params.videoCodecs;
    const auto offered = std::find_if(offeredCodecs.cbegin(), offeredCodecs.cend(), [codec](const Codec &each) { return videoCodecFromName(each.name) == codec && each.encode.valueOr(false); });
    return offered == offeredCodecs.cend() ? nullptr : &*offered;
}

bool VoiceClient::VideoSendPath::prepareSenders()
{
    const Codec *offered = offeredForEncoding(sendCodec);
    if (layers.empty() || !offered || !VideoPacketizer::supports(sendCodec)) {
        for (Layer &layer : layers) {
            if (layer.sender)
                layer.sender->setCodec(VideoCodec::Unknown, 0, std::nullopt);
        }
        return false;
    }

    std::optional<uint8_t> rtxPayloadType;
    if (offered->rtxPayloadType.hasValue())
        rtxPayloadType = static_cast<uint8_t>(offered->rtxPayloadType.get());

    for (Layer &layer : layers) {
        if (!layer.sender) {
            // as WebRTC starts them
            QRandomGenerator *random = QRandomGenerator::global();
            VideoSendStream::Numbering first;
            first.sequence = static_cast<uint16_t>(random->bounded(1, 0x8000));
            first.rtxSequence = static_cast<uint16_t>(random->bounded(1, 0x8000));
            first.pictureId = static_cast<uint16_t>(random->bounded(0x8000));
            first.transportSequence = 1;
            if (std::none_of(layers.cbegin(), layers.cend(), [](const Layer &each) { return each.sender != nullptr; }))
                estimator.restart(first.transportSequence);
            layer.sender = std::make_unique<VideoSendStream>(layer.ssrc, layer.rtxSsrc, first);
            layer.sender->numberWith(&estimator);
            layer.sender->setPacingFactor(client.params.streamContext ? VideoSendStream::SCREEN_PACING_FACTOR : VideoSendStream::CAMERA_PACING_FACTOR);
        }
        layer.sender->setCodec(sendCodec, static_cast<uint8_t>(offered->payloadType.get()), rtxPayloadType);
    }
    paceByBitrate();
    registerWithDave();
    return true;
}

void VoiceClient::VideoSendPath::setBitrate(int bitrateBps, int quality)
{
    Layer *layer = layerOfQuality(quality);
    if (!layer)
        return;
    layer->bitrateBps = bitrateBps;
    paceByBitrate();
}

int VoiceClient::VideoSendPath::announcedMaxBitrate(const Layer &layer) const
{
    return &layer == fullLayer() || !started->secondLayerMaxBitrateBps ? started->maxBitrateBps : *started->secondLayerMaxBitrateBps;
}

void VoiceClient::VideoSendPath::paceByBitrate()
{
    if (!started)
        return;
    for (Layer &layer : layers) {
        if (layer.sender)
            layer.sender->setBitrate(layer.bitrateBps.value_or(announcedMaxBitrate(layer)));
    }
}

void VoiceClient::VideoSendPath::registerWithDave()
{
    if (!client.daveSession)
        return;
    for (const Layer &layer : layers) {
        if (!layer.sender)
            continue;
        if (const std::optional<discord::dave::Codec> codec = daveCodecOf(layer.sender->codec()))
            client.daveSession->setLocalVideoCodec(layer.ssrc, *codec);
    }
}

void VoiceClient::VideoSendPath::start(const LocalVideoDescription &description)
{
    if (client.currentState != State::Connected || !client.gateway || videoSsrc() == 0)
        return;

    const bool starting = !started;
    started = description;
    if (starting) {
        if (!prepareSenders())
            qCWarning(LogVoice) << "Video is announced but cannot be sent as" << videoCodecName(sendCodec);

        sendReportedUs = nowUs();
        for (Layer &layer : layers)
            layer.bytesAtSendReport = layer.sender ? layer.sender->stats().bytesWritten : 0;
        sendReportTimer->start(SEND_REPORT_INTERVAL_MS);
    }
    paceByBitrate();
    announce();
    emitLayersKnown();
}

void VoiceClient::VideoSendPath::describe(const LocalVideoDescription &description)
{
    if (!started)
        return;
    started = description;
    paceByBitrate();
    announce();
}

void VoiceClient::VideoSendPath::stop()
{
    // as the official client, video that was never on isnt announced off
    if (!started)
        return;
    started.reset();

    for (Layer &layer : layers) {
        layer.bitrateBps.reset();
        layer.awaitsKeyframe = false;
        layer.keyframeAskWaiting = false;
        if (layer.sender)
            layer.sender->clear();
    }
    paceTimer->stop();
    sendReportTimer->stop();
    keyframeTimer->stop();
    announceOff();
}

void VoiceClient::VideoSendPath::announce()
{
    if (!client.gateway)
        return;

    StreamResolution resolution;
    resolution.type = started->maxSize ? QStringLiteral("fixed") : QStringLiteral("source");
    resolution.width = started->maxSize ? started->maxSize->width() : 0;
    resolution.height = started->maxSize ? started->maxSize->height() : 0;

    // as the official client: every layer is announced active, at the same size and rate, while video is
    // on
    announcedStreams = streams;
    for (StreamDescription &stream : announcedStreams) {
        const Layer *layer = layerOf(stream.ssrc.valueOr(0));
        stream.active = layer != nullptr;
        if (!layer)
            continue;
        stream.maxBitrate = announcedMaxBitrate(*layer);
        stream.maxFramerate = started->maxFramerate;
        stream.maxResolution = resolution;
    }

    const quint32 ssrc = videoSsrc();
    LocalVideoState state;
    state.audioSsrc = client.localSsrc;
    state.videoSsrc = ssrc;
    // as the official client, always the SSRC after the video one, whatever READY gave
    state.rtxSsrc = ssrc == 0 ? 0 : ssrc + 1;
    state.streams = announcedStreams;
    client.gateway->sendVideo(state);
}

void VoiceClient::VideoSendPath::announceOff()
{
    if (!client.gateway)
        return;

    for (StreamDescription &stream : announcedStreams)
        stream.active = false;
    LocalVideoState state;
    state.audioSsrc = client.localSsrc;
    state.videoSsrc = quint32(0);
    state.rtxSsrc = quint32(0);
    state.streams = announcedStreams;
    client.gateway->sendVideo(state);
}

void VoiceClient::VideoSendPath::sendFrame(const EncodedVideoFrame &frame, int quality)
{
    Layer *layer = layerOfQuality(quality);
    if (!client.canSendMedia() || !started || !layer || !layer->sender)
        return;

    DaveSession *dave = client.daveSession.get();
    if (dave && !dave->isDaveEnabled() && !dave->isDowngraded()) {
        // the official client sends placeholder frames until DAVE keys are agreed; sending nothing works as
        // well
        layer->awaitsKeyframe = true;
        return;
    }
    if (layer->awaitsKeyframe && !frame.keyframe)
        return;

    const qint64 now = nowUs();
    bool queued = false;
    if (dave && dave->isDaveEnabled()) {
        if (!daveEncrypt(frame.data, layer->ssrc, encryptedFrame)) {
            layer->awaitsKeyframe = true;
            askForKeyframe(*layer);
            return;
        }
        queued = layer->sender->queueFrame(encryptedFrame, frame.rtpTimestamp, frame.keyframe, now);
    } else {
        queued = layer->sender->queueFrame(frame.data, frame.rtpTimestamp, frame.keyframe, now);
    }
    if (!queued)
        return;

    layer->awaitsKeyframe = false;
    startPacing();
}

bool VoiceClient::VideoSendPath::daveEncrypt(std::span<const uint8_t> frame, quint32 ssrc, std::vector<uint8_t> &encrypted)
{
    // the encryptor writes its trailer without checking the room it was given, and its size estimate
    // allows only 64 bytes over the frame, while each NAL or OBU can grow by up to nine bytes
    encrypted.resize(client.daveSession->maxCiphertextSize(discord::dave::MediaType::Video, frame.size()) + frame.size() * 5);

    const std::optional<size_t> written = client.daveSession->encrypt(discord::dave::MediaType::Video, ssrc, frame, encrypted);
    if (!written)
        return false;
    encrypted.resize(*written);
    return true;
}

void VoiceClient::VideoSendPath::startPacing()
{
    if (!paceTimer->isActive())
        paceTimer->start(VideoSendStream::PACE_INTERVAL_MS);
}

// writing a whole frame's packets at once overruns queues on the way and comes out as loss
void VoiceClient::VideoSendPath::onPaceTick()
{
    if (!client.canSendMedia() || !anyWaiting()) {
        paceTimer->stop();
        return;
    }

    PacketPool::Handle packet = client.udpTransport->acquirePacket();
    for (Layer &layer : layers) {
        if (!layer.sender || !layer.sender->hasWaiting())
            continue;
        layer.sender->startInterval(nowUs());
        while (layer.sender->writeNext(nowUs(), *packet)) {
            if (client.encryption->seal(*packet, VideoSendStream::CLEAR_SIZE))
                client.udpTransport->send(*packet);
        }
    }
}

void VoiceClient::VideoSendPath::onSendReportTick()
{
    if (!started)
        return;
    const qint64 now = nowUs();
    sendSenderReports(now);
    emitSendStats(now);
}

void VoiceClient::VideoSendPath::sendSenderReports(qint64 now)
{
    if (!client.canSendMedia())
        return;
    for (Layer &layer : layers) {
        if (!layer.sender || !layer.sender->canReport())
            continue;
        PacketPool::Handle packet = client.udpTransport->acquirePacket();
        packet->resize(packet->tailroom() - VoiceEncryption::TRAILER_SIZE);
        const int size = layer.sender->writeSenderReport(packet->bytes(), now, ntpTimeNow());
        if (size > 0 && packet->resize(size) && client.encryption->seal(*packet, RTCP_CLEAR_SIZE))
            client.udpTransport->send(*packet);
    }
}

void VoiceClient::VideoSendPath::emitSendStats(qint64 now)
{
    VideoSendStats stats;
    stats.codec = videoCodecName(sendCodec);
    stats.keyframeRequests = keyframeRequests;
    if (const Layer *full = fullLayer())
        stats.receivers = full->receivers;
    stats.receiverEstimateBps = receiverEstimateBps;
    stats.delayEstimateBps = estimator.estimate();
    stats.acknowledgedBps = estimator.acknowledged();
    reportedDelayEstimateBps = stats.delayEstimateBps;

    quint64 bytesSince = 0;
    for (Layer &layer : layers) {
        if (!layer.sender)
            continue;
        const VideoSendStream::Stats &sent = layer.sender->stats();
        stats.packets += sent.packets;
        stats.bytes += sent.bytes;
        stats.frames += sent.frames;
        stats.keyframes += sent.keyframes;
        stats.retransmitted += sent.retransmitted;
        stats.nacksReceived += sent.nacksReceived;
        stats.lossFraction = worstOf(stats.lossFraction, sent.lossFraction);
        stats.roundTripMs = worstOf(stats.roundTripMs, sent.roundTripMs);
        bytesSince += sent.bytesWritten - layer.bytesAtSendReport;
        layer.bytesAtSendReport = sent.bytesWritten;
    }
    if (now > sendReportedUs)
        stats.bitrateBps = static_cast<quint64>(rateOf(static_cast<qint64>(bytesSince), now - sendReportedUs));
    sendReportedUs = now;

    emit client.videoSendStatsUpdated(stats);
}

void VoiceClient::VideoSendPath::receiveRtcp(const ReceivedRtcp &received)
{
    if (layers.empty())
        return;

    for (const ReceivedRtcp::ReceiverCount &count : received.receiverCounts) {
        if (Layer *layer = layerOf(count.ssrc))
            noteReceivers(*layer, count.receivers);
    }

    if (received.receiverEstimateBps) {
        if (received.receiverEstimateBps != receiverEstimateBps)
            qCDebug(LogVoice) << "Receiving side's estimate of the way from here:" << *received.receiverEstimateBps << "bps";
        receiverEstimateBps = received.receiverEstimateBps;
    }

    if (!started)
        return;

    const qint64 now = nowUs();
    for (const ReceivedRtcp::Nack &nack : received.nacks) {
        if (Layer *layer = layerOf(nack.mediaSsrc); layer && layer->sender)
            layer->sender->retransmit(nack.sequences, now);
    }
    if (anyWaiting())
        startPacing();

    for (const ReceptionReport &report : received.receptionReports) {
        Layer *layer = layerOf(report.ssrc);
        if (!layer || !layer->sender)
            continue;
        layer->sender->onReceptionReport(report, now);
        if (const std::optional<int> roundTripMs = layer->sender->stats().roundTripMs)
            estimator.setRoundTrip(qint64(*roundTripMs) * 1000);
    }

    for (const ReceivedRtcp::TransportReport &report : received.transportReports) {
        if (estimator.reportsTaken() == 0)
            qCInfo(LogVoice) << "The server reports when this client's video packets reach it";
        estimator.onReport(report, now);
    }
    const std::optional<qint64> estimate = estimator.estimate();
    if (estimate && (!reportedDelayEstimateBps || *estimate < *reportedDelayEstimateBps * DELAY_ESTIMATE_STEP) && sendReportTimer->isActive()) {
        qCDebug(LogVoice) << "The way from here is falling behind: it is taken to carry" << *estimate << "bps of the" << estimator.acknowledged().value_or(0) << "the server acknowledges";
        onSendReportTick();
        sendReportTimer->start(SEND_REPORT_INTERVAL_MS);
    }

    for (const uint32_t ssrc : received.keyframeRequests) {
        Layer *layer = layerOf(ssrc);
        if (!layer)
            continue;
        keyframeRequests++;
        askForKeyframe(*layer);
    }
}

void VoiceClient::VideoSendPath::noteReceivers(Layer &layer, int receivers)
{
    if (layer.receivers == receivers)
        return;

    const bool firstReceiver = layer.receivers.value_or(0) <= 0 && receivers > 0;
    layer.receivers = receivers;
    emit client.videoReceiversChanged(receivers, layer.quality);
    if (firstReceiver)
        askForKeyframe(layer);
}

void VoiceClient::VideoSendPath::askEveryLayerForKeyframe()
{
    askLayersForKeyframe([](const Layer &) { return true; });
}

void VoiceClient::VideoSendPath::askAwaitingLayersForKeyframe()
{
    askLayersForKeyframe([](const Layer &layer) { return layer.awaitsKeyframe; });
}

void VoiceClient::VideoSendPath::askLayersForKeyframe(const std::function<bool(const Layer &)> &which)
{
    for (Layer &layer : layers) {
        if (which(layer))
            askForKeyframe(layer);
    }
}

void VoiceClient::VideoSendPath::askForKeyframe(Layer &layer)
{
    if (!started)
        return;

    const qint64 now = clock.elapsed();
    const qint64 wait = layer.keyframeAskedMs ? *layer.keyframeAskedMs + KEYFRAME_INTERVAL_MS - now : 0;
    if (wait > 0) {
        layer.keyframeAskWaiting = true;
        if (!keyframeTimer->isActive())
            keyframeTimer->start(static_cast<int>(wait));
        return;
    }

    layer.keyframeAskWaiting = false;
    layer.keyframeAskedMs = now;
    emit client.videoKeyframeRequested(layer.quality);
}

// a want of nothing reads as a want of everything, as in the official client
void VoiceClient::VideoSendPath::onMediaSinkWants(const MediaSinkWants &wants)
{
    for (const Layer &layer : layers) {
        int want = FULL_QUALITY;
        if (const auto own = wants.bySsrc.constFind(layer.ssrc); own != wants.bySsrc.constEnd() && own.value() > 0)
            want = own.value();
        else if (wants.any && *wants.any > 0)
            want = *wants.any;

        const auto pixelCount = wants.pixelCounts.constFind(layer.ssrc);
        emit client.videoWantsReceived(want, pixelCount != wants.pixelCounts.constEnd() ? pixelCount.value() : 0, layer.quality);
    }
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
