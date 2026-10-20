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

// The wall clock as a sender report states it: seconds since 1900 over a binary fraction.
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

} // namespace

VoiceClient::VideoSendPath::VideoSendPath(VoiceClient &client) : client(client)
{
    clock.start();
}

VoiceClient::VideoSendPath::~VideoSendPath()
{
    delete paceTimer;
    delete sendReportTimer;
    delete keyframeTimer;
}

void VoiceClient::VideoSendPath::reset()
{
    delete std::exchange(paceTimer, nullptr);
    delete std::exchange(sendReportTimer, nullptr);
    delete std::exchange(keyframeTimer, nullptr);
    for (Layer &layer : layers) {
        Layer fresh;
        fresh.quality = layer.quality;
        fresh.ssrc = layer.ssrc;
        fresh.rtxSsrc = layer.rtxSsrc;
        layer = std::move(fresh);
    }
    estimator = SendSideEstimator();
    reportedDelayEstimateBps = 0;
    started.reset();
    receiverEstimateBps = 0;
    announcedStreams.clear();
    keyframeRequests = 0;
}

void VoiceClient::VideoSendPath::takeStreams(const VoiceReady &ready)
{
    streams = ready.streams.valueOr();
    if (streams.isEmpty()) {
        // Given none, the official client takes the two SSRCs that follow its audio one.
        StreamDescription stream;
        stream.type = QStringLiteral("video");
        stream.rid = QStringLiteral("100");
        stream.ssrc = client.localSsrc + 1;
        stream.quality = FULL_QUALITY;
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
        layer.rtxSsrc = stream.rtxSsrc.hasValue() && stream.rtxSsrc.get() != 0 ? stream.rtxSsrc.get() : ssrc + 1;
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

// The official client's limits for what it captures go to the full-quality layer, and its
// SSRC is the one an announcement names at the top.
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

    // The server picks among the codecs this client said it encodes.
    VideoCodec codec = VideoCodec::Unknown;
    for (const Codec &offered : client.params.videoCodecs) {
        if (offered.name.get() == name && offered.encode.valueOr(false))
            codec = videoCodecFromName(name);
    }
    if (codec == sendCodec)
        return;

    qCInfo(LogVoice) << "Video is to be sent as" << (codec == VideoCodec::Unknown ? QStringLiteral("nothing this client encodes") : name);
    sendCodec = codec;
    if (started) {
        prepareSenders();
        // Frames of the old codec may still be on their way here.
        for (Layer &layer : layers)
            layer.awaitsKeyframe = true;
    }
    emit client.videoSendCodecChanged(codec);
}

bool VoiceClient::VideoSendPath::prepareSenders()
{
    const QList<Codec> &offeredCodecs = client.params.videoCodecs;
    const auto offered = std::find_if(offeredCodecs.cbegin(), offeredCodecs.cend(), [this](const Codec &codec) { return videoCodecFromName(codec.name) == sendCodec && codec.encode.valueOr(false); });
    if (layers.empty() || offered == offeredCodecs.cend() || !VideoPacketizer::supports(sendCodec)) {
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
            // As WebRTC starts them: the sequence numbers and the picture id anywhere in the
            // lower half of their range, the transport-wide numbering at one. That one is the
            // connection's, and runs through every layer's packets.
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
    return &layer == fullLayer() || started->secondLayerMaxBitrate <= 0 ? started->maxBitrate : started->secondLayerMaxBitrate;
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
        if (!sendReportTimer) {
            sendReportTimer = new QTimer(&client);
            QObject::connect(sendReportTimer, &QTimer::timeout, &client, [this] { onSendReportTick(); });
        }
        sendReportTimer->start(SEND_REPORT_INTERVAL_MS);
    }
    paceByBitrate();
    announce();
    // Said again for whoever began to listen after the connection was made.
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
    // Video that was never on is not said to be off either: the official client tells the
    // server nothing of its video until it has been on once.
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
    for (QTimer *timer : { paceTimer, sendReportTimer, keyframeTimer }) {
        if (timer)
            timer->stop();
    }
    announceOff();
}

void VoiceClient::VideoSendPath::announce()
{
    if (!client.gateway)
        return;

    StreamResolution resolution;
    const bool sourceSized = started->maxWidth == 0 && started->maxHeight == 0;
    resolution.type = sourceSized ? QStringLiteral("source") : QStringLiteral("fixed");
    resolution.width = started->maxWidth;
    resolution.height = started->maxHeight;

    // The official client says of every layer that it is active while it sends video at all,
    // and announces the same size and frame rate for each: which of them is being encoded is
    // between the server and its native module, and goes by who receives it.
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
    // Always the SSRC after the video one, as the official client has it, whatever READY gave
    // the layer for its retransmissions.
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
    if (client.currentState != State::Connected || !client.encryption || !client.udpTransport || !started || !layer || !layer->sender)
        return;

    DaveSession *dave = client.daveSession.get();
    if (dave && !dave->isDaveEnabled() && !dave->isDowngraded()) {
        // Keys are still being agreed, and no picture goes out without them. The official
        // client fills the gap with placeholder frames; sending nothing serves as well.
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

// The whole frame is encrypted before it is cut into packets, and comes out in another shape:
// the encryptor appends its trailer, widens the short start codes of H264 and H265, and of AV1
// drops the OBUs that are not sent and the size field of the last one.
bool VoiceClient::VideoSendPath::daveEncrypt(std::span<const uint8_t> frame, quint32 ssrc, std::vector<uint8_t> &encrypted)
{
    auto *encryptor = client.daveSession->encryptor();
    // The encryptor writes its trailer without looking at how much room it was given, and the
    // size it asks for allows 64 bytes beyond the frame. A frame of many units outgrows that:
    // each NAL or OBU can add a widened start code and a pair of range numbers, nine bytes at
    // most, to a unit that may be only two bytes long.
    encrypted.resize(encryptor->GetMaxCiphertextByteSize(discord::dave::MediaType::Video, frame.size()) + frame.size() * 5);

    size_t bytesWritten = 0;
    const auto result = encryptor->Encrypt(discord::dave::MediaType::Video, ssrc, discord::dave::ArrayView<const uint8_t>(frame.data(), frame.size()), discord::dave::ArrayView<uint8_t>(encrypted.data(), encrypted.size()), &bytesWritten);
    if (result != discord::dave::IEncryptor::Success) {
        qCWarning(LogDave) << "DAVE video encrypt failed: result =" << static_cast<int>(result) << "frameSize =" << frame.size();
        return false;
    }
    encrypted.resize(bytesWritten);
    return true;
}

void VoiceClient::VideoSendPath::startPacing()
{
    if (!paceTimer) {
        paceTimer = new QTimer(&client);
        paceTimer->setTimerType(Qt::PreciseTimer);
        QObject::connect(paceTimer, &QTimer::timeout, &client, [this] { onPaceTick(); });
    }
    if (!paceTimer->isActive())
        paceTimer->start(VideoSendStream::PACE_INTERVAL_MS);
}

// A frame's packets written to the socket all at once overrun queues on the way and come out
// the far end as loss, so each tick lets only a share of them go.
void VoiceClient::VideoSendPath::onPaceTick()
{
    if (client.currentState != State::Connected || !client.encryption || !client.udpTransport || !anyWaiting()) {
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
    const Layer *full = fullLayer();

    VideoSendStats stats;
    stats.codec = videoCodecName(sendCodec);
    stats.keyframeRequests = keyframeRequests;
    stats.receivers = full ? full->receivers.value_or(-1) : -1;
    stats.receiverEstimateBps = receiverEstimateBps;
    stats.delayEstimateBps = static_cast<quint64>(estimator.estimate());
    stats.acknowledgedBps = static_cast<quint64>(estimator.acknowledged());
    reportedDelayEstimateBps = stats.delayEstimateBps;

    // What every layer sent, taken together.
    quint64 bytesSince = 0;
    for (Layer &layer : layers) {
        if (!layer.sender)
            continue;
        if (layer.sender->canReport() && client.currentState == State::Connected && client.encryption && client.udpTransport) {
            PacketPool::Handle packet = client.udpTransport->acquirePacket();
            packet->resize(packet->tailroom() - VoiceEncryption::TRAILER_SIZE);
            const int size = layer.sender->writeSenderReport(packet->bytes(), now, ntpTimeNow());
            if (size > 0 && packet->resize(size) && client.encryption->seal(*packet, RTCP_CLEAR_SIZE))
                client.udpTransport->send(*packet);
        }

        const VideoSendStream::Stats &sent = layer.sender->stats();
        stats.packets += sent.packets;
        stats.bytes += sent.bytes;
        stats.frames += sent.frames;
        stats.keyframes += sent.keyframes;
        stats.retransmitted += sent.retransmitted;
        stats.nacksReceived += sent.nacksReceived;
        stats.lossFraction = std::max(stats.lossFraction, sent.lossFraction);
        stats.roundTripMs = std::max(stats.roundTripMs, sent.roundTripMs);
        bytesSince += sent.bytesWritten - layer.bytesAtSendReport;
        layer.bytesAtSendReport = sent.bytesWritten;
    }
    if (now > sendReportedUs)
        stats.bitrateBps = bytesSince * 8 * 1000000 / static_cast<quint64>(now - sendReportedUs);
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

    if (received.hasReceiverEstimate) {
        if (received.receiverEstimateBitsPerSecond != receiverEstimateBps)
            qCDebug(LogVoice) << "Receiving side's estimate of the way from here:" << received.receiverEstimateBitsPerSecond << "bps";
        receiverEstimateBps = received.receiverEstimateBitsPerSecond;
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
        if (layer->sender->stats().roundTripMs >= 0)
            estimator.setRoundTrip(qint64(layer->sender->stats().roundTripMs) * 1000);
    }

    for (const ReceivedRtcp::TransportReport &report : received.transportReports) {
        if (estimator.reportsTaken() == 0)
            qCInfo(LogVoice) << "The server reports when this client's video packets reach it";
        estimator.onReport(report, now);
    }
    // Whoever sets the encoder's bitrate hears of it with the statistics, which a way that
    // has just been found to fall behind is not left to wait for.
    const quint64 estimate = static_cast<quint64>(estimator.estimate());
    if (estimate > 0 && (reportedDelayEstimateBps == 0 || estimate < reportedDelayEstimateBps * DELAY_ESTIMATE_STEP) && sendReportTimer && sendReportTimer->isActive()) {
        qCDebug(LogVoice) << "The way from here is falling behind: it is taken to carry" << estimate << "bps of the" << estimator.acknowledged() << "the server acknowledges";
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

    // Whoever has only now begun to receive has seen none of what was sent so far.
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
        // Too soon after the last one: this request is passed on when the interval is over,
        // together with any others that come until then.
        layer.keyframeAskWaiting = true;
        if (!keyframeTimer) {
            keyframeTimer = new QTimer(&client);
            keyframeTimer->setSingleShot(true);
            QObject::connect(keyframeTimer, &QTimer::timeout, &client, [this] { askLayersForKeyframe([](const Layer &layer) { return layer.keyframeAskWaiting; }); });
        }
        if (!keyframeTimer->isActive())
            keyframeTimer->start(static_cast<int>(wait));
        return;
    }

    layer.keyframeAskWaiting = false;
    layer.keyframeAskedMs = now;
    emit client.videoKeyframeRequested(layer.quality);
}

// What is said of a layer's own SSRC counts before what is said of any, and when neither
// says anything all of it is wanted: a want of nothing is read as a want of everything, as
// the official client reads it.
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
