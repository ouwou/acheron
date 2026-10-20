// The voice client's video: what it receives and hands on, what it sends, and what it tells
// the server of either. A build without cameras and screen shares has
// VoiceClientWithoutVideo.cpp in this file's place.

#include "VoiceClient.hpp"
#include "VoiceGateway.hpp"
#include "UdpTransport.hpp"
#include "VoiceEncryption.hpp"
#include "RtpPacket.hpp"
#include "Rtcp.hpp"
#include "VideoSendStream.hpp"

#include "DaveSession.hpp"

#include "Core/Logging.hpp"

#include <QDateTime>
#include <QRandomGenerator>
#include <QtEndian>

#include <algorithm>
#include <utility>

namespace Acheron {
namespace Discord {
namespace Voice {

// The wall clock as a sender report states it: seconds since 1900 over a binary fraction.
static uint64_t ntpTimeNow()
{
    constexpr quint64 SECONDS_FROM_1900_TO_1970 = 2208988800ULL;
    const quint64 ms = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    return ((ms / 1000 + SECONDS_FROM_1900_TO_1970) << 32) | (((ms % 1000) << 32) / 1000);
}

void VoiceClient::cleanupVideo()
{
    if (videoTimer) {
        videoTimer->stop();
        delete videoTimer;
        videoTimer = nullptr;
    }
    remoteVideos.clear();
    rtxToPrimary.clear();
    remoteLayers.clear();
    deliveredSsrcs.clear();
    sentVideoWants.clear();
    transportFeedback = TransportFeedback();
    senderReports.clear();
    serverEstimateBps = 0;
    serverLossFraction = 0;

    delete videoPaceTimer;
    videoPaceTimer = nullptr;
    delete videoSendReportTimer;
    videoSendReportTimer = nullptr;
    delete localKeyframeTimer;
    localKeyframeTimer = nullptr;
    for (LocalVideoLayer &layer : localLayers) {
        LocalVideoLayer fresh;
        fresh.quality = layer.quality;
        fresh.ssrc = layer.ssrc;
        fresh.rtxSsrc = layer.rtxSsrc;
        layer = std::move(fresh);
    }
    sendEstimator = SendSideEstimator();
    reportedDelayEstimateBps = 0;
    localVideo.reset();
    videoReceiverEstimateBps = 0;
    announcedVideoStreams.clear();
    localKeyframeRequests = 0;
}

void VoiceClient::setVideoFrameHandler(VideoFrameHandler handler)
{
    videoFrameHandler = std::move(handler);
}

void VoiceClient::readVideoPayloadTypes()
{
    for (const Codec &codec : params.videoCodecs) {
        const VideoCodec known = videoCodecFromName(codec.name);
        if (known == VideoCodec::Unknown || !codec.decode.valueOr(false))
            continue;
        videoPayloadTypes.insert(codec.payloadType, known);
        if (codec.rtxPayloadType.hasValue())
            rtxPayloadTypes.insert(codec.rtxPayloadType.get(), known);
    }
}

void VoiceClient::noteTransportSequence(std::span<const uint8_t> extensions)
{
    // Only while feedback is going out; otherwise the record would grow without end.
    if (!videoTimer || !videoTimer->isActive())
        return;
    const std::span<const uint8_t> sequence = findRtpExtension(extensions, TRANSPORT_SEQUENCE_EXTENSION);
    if (sequence.size() == 2)
        transportFeedback.onPacket(qFromBigEndian<uint16_t>(sequence.data()), videoClock.nsecsElapsed() / 1000);
}

void VoiceClient::receiveVideo(const ParsedRtp &rtp, PacketPool::Handle packet, VideoCodec codec, bool retransmission)
{
    const RtpHeader &header = rtp.header;
    if (!encryption || !encryption->open(*packet, rtp.clearSize))
        return;

    packet->dropFront(rtp.clearSize);
    if (packet->size() < rtp.extensionSize)
        return;
    // Every packet the server numbered is reported back to it, wanted or not, since the
    // numbering is about the connection and not about any one stream.
    noteTransportSequence(packet->bytes().first(static_cast<size_t>(rtp.extensionSize)));
    transportMediaSsrc = header.ssrc;
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
        auto primary = rtxToPrimary.constFind(ssrc);
        if (primary == rtxToPrimary.constEnd() || packet->size() <= 2)
            return;
        ssrc = primary.value();
        sequence = qFromBigEndian<uint16_t>(packet->data());
        packet->dropFront(2);
    }

    auto it = remoteVideos.find(ssrc);
    if (it == remoteVideos.end() || packet->size() == 0) {
        unrouted.rtp++;
        return;
    }
    if (!deliveredSsrcs.contains(ssrc))
        return;

    RemoteVideo &video = it->second;
    if (!video.receiver || video.receiver->codec() != codec)
        video.receiver = std::make_unique<VideoReceiver>(ssrc, codec);

    video.receiver->insert(sequence, header.timestamp, header.marker, std::move(packet), videoClock.elapsed(), retransmission);
    deliverVideoFrames(ssrc, video);
}

void VoiceClient::deliverVideoFrames(quint32 ssrc, RemoteVideo &video)
{
    std::vector<EncodedVideoFrame> frames = video.receiver->takeFrames();
    const VideoCodec codec = video.receiver->codec();

    for (EncodedVideoFrame &frame : frames) {
        if (daveSession) {
            if (daveSession->isDaveEnabled()) {
                if (!daveDecryptVideo(ssrc, frame.data)) {
                    // Later frames build on this one, so they are of no use either.
                    videoDecryptFailures++;
                    qCDebug(LogDave) << "Undecryptable" << describeFrame(codec, frame.data);
                    video.receiver->requestKeyframe();
                    return;
                }
            } else if (!daveSession->isDowngraded()) {
                // Keys are still being agreed. A key frame is asked for once they are.
                videoDroppedAwaitingDaveKeys = true;
                return;
            }
        }

        prepareForDecoder(codec, frame.data);
        if (videoFrameHandler)
            videoFrameHandler(video.userId, codec, std::move(frame));
    }
}

bool VoiceClient::daveDecryptVideo(quint32 ssrc, std::vector<uint8_t> &frame)
{
    auto *decryptor = daveSession->getOrCreateDecryptor(ssrc, ssrcToUserIdMap.value(ssrc, 0));

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

void VoiceClient::applyRemoteVideo(const ClientConnectData &data)
{
    if (videoPayloadTypes.isEmpty() || !data.userId.hasValue())
        return;
    const Core::Snowflake userId = data.userId.get();
    if (!userId.isValid() || userId == params.userId)
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
                layer.quality = stream.quality.valueOr(100);
                if (!stream.active.valueOr(true)) {
                    flaggedInactive.append(layer);
                    continue;
                }
                layers.append(layer);

                if (layer.quality == 100 || maxWidth == 0) {
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
        if (layers.isEmpty() && !params.streamContext)
            layers = flaggedInactive;
        if (layers.isEmpty() && (!data.streams.hasValue() || data.streams->isEmpty())) {
            VideoLayer layer;
            layer.ssrc = data.videoSsrc;
            layer.rtxSsrc = layer.ssrc + 1;
            layers.append(layer);
        }
    }

    if (remoteLayers.value(userId) != layers) {
        removeRemoteVideo(userId);
        for (const VideoLayer &layer : layers) {
            RemoteVideo video;
            video.userId = userId;
            video.rtxSsrc = layer.rtxSsrc;
            remoteVideos.emplace(layer.ssrc, std::move(video));
            rtxToPrimary.insert(layer.rtxSsrc, layer.ssrc);

            ssrcToUserIdMap.insert(layer.ssrc, userId);
            if (daveSession)
                daveSession->applyKeyRatchetForSsrc(layer.ssrc, userId);
        }
        if (!layers.isEmpty())
            remoteLayers.insert(userId, layers);
    }

    // On a voice connection whoever shows the video decides what is wanted, and is told here.
    if (!explicitVideoPlan && params.streamContext)
        planSingleSenders();
    emit remoteVideoLayersChanged(userId, layers);
    emit remoteVideoChanged(userId, !layers.isEmpty(), maxWidth, maxHeight, maxFramerate);
}

void VoiceClient::removeRemoteVideo(Core::Snowflake userId)
{
    for (auto it = remoteVideos.begin(); it != remoteVideos.end();) {
        if (it->second.userId != userId) {
            ++it;
            continue;
        }
        rtxToPrimary.remove(it->second.rtxSsrc);
        ssrcToUserIdMap.remove(it->first);
        deliveredSsrcs.remove(it->first);
        it = remoteVideos.erase(it);
    }
    remoteLayers.remove(userId);
}

void VoiceClient::setVideoWants(int pixelCount, bool wanted)
{
    explicitVideoPlan = false;
    wantedPixelCount = pixelCount;
    videoWanted = wanted;
    planSingleSenders();
}

// The plan for a stream connection, where the only sender is the broadcaster: its first
// encoding at full quality, sized to the surface, or nothing while the surface is not shown.
void VoiceClient::planSingleSenders()
{
    plannedWants = MediaSinkWants();
    deliveredSsrcs.clear();
    for (auto it = remoteLayers.constBegin(); it != remoteLayers.constEnd(); ++it) {
        const quint32 ssrc = it.value().first().ssrc;
        plannedWants.bySsrc.insert(ssrc, videoWanted ? 100 : 0);
        if (videoWanted) {
            plannedWants.pixelCounts.insert(ssrc, wantedPixelCount);
            deliveredSsrcs.insert(ssrc);
        }
    }
    applyVideoPlan();
}

void VoiceClient::setVideoPlan(const MediaSinkWants &wants, const QSet<quint32> &deliver)
{
    explicitVideoPlan = true;
    plannedWants = wants;
    deliveredSsrcs = deliver;
    applyVideoPlan();
}

void VoiceClient::applyVideoPlan()
{
    // An encoding no longer taken starts from a key frame if it is taken again.
    for (auto &[ssrc, video] : remoteVideos) {
        if (!deliveredSsrcs.contains(ssrc))
            video.receiver.reset();
    }
    sendVideoWants();
}

VideoCodec VoiceClient::videoSendCodec() const
{
    return sendCodec;
}

void VoiceClient::takeLocalVideoStreams(const VoiceReady &ready)
{
    localVideoStreams = ready.streams.valueOr();
    if (localVideoStreams.isEmpty()) {
        // Given none, the official client takes the two SSRCs that follow its audio one.
        StreamDescription stream;
        stream.type = QStringLiteral("video");
        stream.rid = QStringLiteral("100");
        stream.ssrc = localSsrc + 1;
        stream.quality = 100;
        stream.rtxSsrc = localSsrc + 2;
        localVideoStreams.append(stream);
    }

    localLayers.clear();
    QList<int> qualities;
    for (const StreamDescription &stream : std::as_const(localVideoStreams)) {
        const quint32 ssrc = stream.ssrc.valueOr(0);
        if (ssrc == 0)
            continue;
        LocalVideoLayer layer;
        layer.quality = stream.quality.valueOr(100);
        layer.ssrc = ssrc;
        layer.rtxSsrc = stream.rtxSsrc.hasValue() && stream.rtxSsrc.get() != 0 ? stream.rtxSsrc.get() : ssrc + 1;
        qualities.append(layer.quality);
        localLayers.push_back(std::move(layer));
    }
    emit videoSendLayersKnown(qualities);
}

VoiceClient::LocalVideoLayer *VoiceClient::localLayer(int quality)
{
    const auto found = std::find_if(localLayers.begin(), localLayers.end(), [quality](const LocalVideoLayer &layer) { return layer.quality == quality; });
    return found == localLayers.end() ? nullptr : &*found;
}

VoiceClient::LocalVideoLayer *VoiceClient::localLayerOf(quint32 ssrc)
{
    const auto found = std::find_if(localLayers.begin(), localLayers.end(), [ssrc](const LocalVideoLayer &layer) { return layer.ssrc == ssrc; });
    return found == localLayers.end() ? nullptr : &*found;
}

// The official client's limits for what it captures go to the full-quality layer, and its
// SSRC is the one an announcement names at the top.
const VoiceClient::LocalVideoLayer *VoiceClient::fullLayer() const
{
    const auto full = std::find_if(localLayers.cbegin(), localLayers.cend(), [](const LocalVideoLayer &layer) { return layer.quality == FULL_QUALITY; });
    if (full != localLayers.cend())
        return &*full;
    return localLayers.empty() ? nullptr : &localLayers.front();
}

quint32 VoiceClient::localVideoSsrc() const
{
    const LocalVideoLayer *full = fullLayer();
    return full ? full->ssrc : 0;
}

void VoiceClient::offerVideoEncoding(const QList<VideoCodec> &encodable)
{
    bool changed = false;
    for (Codec &offered : params.videoCodecs) {
        const bool encoded = encodable.contains(videoCodecFromName(offered.name));
        if (offered.encode.valueOr(false) == encoded)
            continue;
        offered.encode = encoded;
        changed = true;
    }
    if (!changed)
        return;

    if (currentState == State::WaitingForSession)
        codecsChangedSinceOffered = true;
    if (currentState != State::Connected)
        return;

    sendOfferedCodecs();
    // The server may have named this codec already, when it could not be taken up.
    setVideoSendCodec(serverVideoCodec);
}

void VoiceClient::setVideoSendCodec(const QString &name)
{
    serverVideoCodec = name;

    // The server picks among the codecs this client said it encodes.
    VideoCodec codec = VideoCodec::Unknown;
    for (const Codec &offered : params.videoCodecs) {
        if (offered.name.get() == name && offered.encode.valueOr(false))
            codec = videoCodecFromName(name);
    }
    if (codec == sendCodec)
        return;

    qCInfo(LogVoice) << "Video is to be sent as" << (codec == VideoCodec::Unknown ? QStringLiteral("nothing this client encodes") : name);
    sendCodec = codec;
    if (localVideo) {
        prepareVideoSender();
        // Frames of the old codec may still be on their way here.
        for (LocalVideoLayer &layer : localLayers)
            layer.awaitsKeyframe = true;
    }
    emit videoSendCodecChanged(codec);
}

bool VoiceClient::prepareVideoSender()
{
    const auto offered = std::find_if(params.videoCodecs.cbegin(), params.videoCodecs.cend(), [this](const Codec &codec) { return videoCodecFromName(codec.name) == sendCodec && codec.encode.valueOr(false); });
    if (localLayers.empty() || offered == params.videoCodecs.cend() || !VideoPacketizer::supports(sendCodec)) {
        for (LocalVideoLayer &layer : localLayers) {
            if (layer.sender)
                layer.sender->setCodec(VideoCodec::Unknown, 0, std::nullopt);
        }
        return false;
    }

    std::optional<uint8_t> rtxPayloadType;
    if (offered->rtxPayloadType.hasValue())
        rtxPayloadType = static_cast<uint8_t>(offered->rtxPayloadType.get());

    for (LocalVideoLayer &layer : localLayers) {
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
            if (std::none_of(localLayers.cbegin(), localLayers.cend(), [](const LocalVideoLayer &each) { return each.sender != nullptr; }))
                sendEstimator.restart(first.transportSequence);
            layer.sender = std::make_unique<VideoSendStream>(layer.ssrc, layer.rtxSsrc, first);
            layer.sender->numberWith(&sendEstimator);
            layer.sender->setPacingFactor(params.streamContext ? VideoSendStream::SCREEN_PACING_FACTOR : VideoSendStream::CAMERA_PACING_FACTOR);
        }
        layer.sender->setCodec(sendCodec, static_cast<uint8_t>(offered->payloadType.get()), rtxPayloadType);
    }
    paceVideoByBitrate();
    registerLocalVideoWithDave();
    return true;
}

void VoiceClient::setVideoBitrate(int bitsPerSecond, int quality)
{
    LocalVideoLayer *layer = localLayer(quality);
    if (!layer)
        return;
    layer->bitrate = bitsPerSecond;
    paceVideoByBitrate();
}

int VoiceClient::announcedMaxBitrate(const LocalVideoLayer &layer) const
{
    return &layer == fullLayer() || localVideo->secondLayerMaxBitrate <= 0 ? localVideo->maxBitrate : localVideo->secondLayerMaxBitrate;
}

void VoiceClient::paceVideoByBitrate()
{
    if (!localVideo)
        return;
    for (LocalVideoLayer &layer : localLayers) {
        if (layer.sender)
            layer.sender->setBitrate(layer.bitrate > 0 ? layer.bitrate : announcedMaxBitrate(layer));
    }
}

// The official client registers the codec for every layer's SSRC, and its retransmissions
// need nothing: a frame is encrypted before it is cut into packets.
void VoiceClient::registerLocalVideoWithDave()
{
    if (!daveSession)
        return;

    for (const LocalVideoLayer &layer : localLayers) {
        if (!layer.sender)
            continue;
        switch (layer.sender->codec()) {
        case VideoCodec::H264:
            daveSession->setLocalVideoCodec(layer.ssrc, discord::dave::Codec::H264);
            break;
        case VideoCodec::H265:
            daveSession->setLocalVideoCodec(layer.ssrc, discord::dave::Codec::H265);
            break;
        case VideoCodec::VP8:
            daveSession->setLocalVideoCodec(layer.ssrc, discord::dave::Codec::VP8);
            break;
        case VideoCodec::AV1:
            daveSession->setLocalVideoCodec(layer.ssrc, discord::dave::Codec::AV1);
            break;
        case VideoCodec::Unknown:
            break;
        }
    }
}

void VoiceClient::startVideo(const LocalVideoDescription &description)
{
    if (currentState != State::Connected || !gateway || localVideoSsrc() == 0)
        return;

    const bool starting = !localVideo;
    localVideo = description;
    if (starting) {
        if (!videoClock.isValid())
            videoClock.start();
        if (!prepareVideoSender())
            qCWarning(LogVoice) << "Video is announced but cannot be sent as" << videoCodecName(sendCodec);

        videoSendReportedUs = videoClock.nsecsElapsed() / 1000;
        for (LocalVideoLayer &layer : localLayers)
            layer.bytesAtSendReport = layer.sender ? layer.sender->stats().bytesWritten : 0;
        if (!videoSendReportTimer) {
            videoSendReportTimer = new QTimer(this);
            connect(videoSendReportTimer, &QTimer::timeout, this, &VoiceClient::onVideoSendReportTick);
        }
        videoSendReportTimer->start(VIDEO_SEND_REPORT_INTERVAL_MS);
    }
    paceVideoByBitrate();
    announceLocalVideo(true);

    // Said again for whoever began to listen after the connection was made.
    QList<int> qualities;
    for (const LocalVideoLayer &layer : localLayers)
        qualities.append(layer.quality);
    emit videoSendLayersKnown(qualities);
}

void VoiceClient::describeVideo(const LocalVideoDescription &description)
{
    if (!localVideo)
        return;
    localVideo = description;
    paceVideoByBitrate();
    announceLocalVideo(true);
}

void VoiceClient::stopVideo()
{
    // Video that was never on is not said to be off either: the official client tells the
    // server nothing of its video until it has been on once.
    if (!localVideo)
        return;
    localVideo.reset();

    for (LocalVideoLayer &layer : localLayers) {
        layer.bitrate = 0;
        layer.awaitsKeyframe = false;
        layer.keyframeAskWaiting = false;
        if (layer.sender)
            layer.sender->clear();
    }
    for (QTimer *timer : { videoPaceTimer, videoSendReportTimer, localKeyframeTimer }) {
        if (timer)
            timer->stop();
    }
    announceLocalVideo(false);
}

void VoiceClient::announceLocalVideo(bool on)
{
    if (!gateway)
        return;

    if (on) {
        StreamResolution resolution;
        const bool sourceSized = localVideo->maxWidth == 0 && localVideo->maxHeight == 0;
        resolution.type = sourceSized ? QStringLiteral("source") : QStringLiteral("fixed");
        resolution.width = localVideo->maxWidth;
        resolution.height = localVideo->maxHeight;

        // The official client says of every layer that it is active while it sends video at
        // all, and announces the same size and frame rate for each: which of them is being
        // encoded is between the server and its native module, and goes by who receives it.
        announcedVideoStreams = localVideoStreams;
        for (StreamDescription &stream : announcedVideoStreams) {
            const LocalVideoLayer *layer = localLayerOf(stream.ssrc.valueOr(0));
            stream.active = layer != nullptr;
            if (!layer)
                continue;
            stream.maxBitrate = announcedMaxBitrate(*layer);
            stream.maxFramerate = localVideo->maxFramerate;
            stream.maxResolution = resolution;
        }
    } else {
        for (StreamDescription &stream : announcedVideoStreams)
            stream.active = false;
    }

    const quint32 videoSsrc = on ? localVideoSsrc() : 0;
    LocalVideoState state;
    state.audioSsrc = localSsrc;
    state.videoSsrc = videoSsrc;
    // Always the SSRC after the video one, as the official client has it, whatever READY gave
    // the layer for its retransmissions.
    state.rtxSsrc = videoSsrc == 0 ? 0 : videoSsrc + 1;
    state.streams = announcedVideoStreams;
    gateway->sendVideo(state);
}

void VoiceClient::sendVideoFrame(const EncodedVideoFrame &frame, int quality)
{
    LocalVideoLayer *layer = localLayer(quality);
    if (currentState != State::Connected || !encryption || !udpTransport || !localVideo || !layer || !layer->sender)
        return;

    if (daveSession && !daveSession->isDaveEnabled() && !daveSession->isDowngraded()) {
        // Keys are still being agreed, and no picture goes out without them. The official
        // client fills the gap with placeholder frames; sending nothing serves as well.
        layer->awaitsKeyframe = true;
        return;
    }
    if (layer->awaitsKeyframe && !frame.keyframe)
        return;

    const qint64 now = videoClock.nsecsElapsed() / 1000;
    bool queued = false;
    if (isDaveEnabled()) {
        std::vector<uint8_t> encrypted;
        if (!daveEncryptVideo(frame.data, layer->ssrc, encrypted)) {
            layer->awaitsKeyframe = true;
            askLayerForKeyframe(*layer);
            return;
        }
        queued = layer->sender->queueFrame(encrypted, frame.rtpTimestamp, frame.keyframe, now);
    } else {
        queued = layer->sender->queueFrame(frame.data, frame.rtpTimestamp, frame.keyframe, now);
    }
    if (!queued)
        return;

    layer->awaitsKeyframe = false;
    startVideoPacing();
}

// The whole frame is encrypted before it is cut into packets, and comes out in another shape:
// the encryptor appends its trailer, widens the short start codes of H264 and H265, and of AV1
// drops the OBUs that are not sent and the size field of the last one.
bool VoiceClient::daveEncryptVideo(std::span<const uint8_t> frame, quint32 ssrc, std::vector<uint8_t> &encrypted)
{
    auto *encryptor = daveSession->encryptor();
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

void VoiceClient::startVideoPacing()
{
    if (!videoPaceTimer) {
        videoPaceTimer = new QTimer(this);
        videoPaceTimer->setTimerType(Qt::PreciseTimer);
        connect(videoPaceTimer, &QTimer::timeout, this, &VoiceClient::onVideoPaceTick);
    }
    if (!videoPaceTimer->isActive())
        videoPaceTimer->start(VideoSendStream::PACE_INTERVAL_MS);
}

// A frame's packets written to the socket all at once overrun queues on the way and come out
// the far end as loss, so each tick lets only a share of them go.
void VoiceClient::onVideoPaceTick()
{
    const bool waiting = std::any_of(localLayers.cbegin(), localLayers.cend(), [](const LocalVideoLayer &layer) { return layer.sender && layer.sender->hasWaiting(); });
    if (currentState != State::Connected || !encryption || !udpTransport || !waiting) {
        videoPaceTimer->stop();
        return;
    }

    PacketPool::Handle packet = udpTransport->acquirePacket();
    for (LocalVideoLayer &layer : localLayers) {
        if (!layer.sender || !layer.sender->hasWaiting())
            continue;
        layer.sender->startInterval(videoClock.nsecsElapsed() / 1000);
        while (layer.sender->writeNext(videoClock.nsecsElapsed() / 1000, *packet)) {
            if (encryption->seal(*packet, VideoSendStream::CLEAR_SIZE))
                udpTransport->send(*packet);
        }
    }
}

void VoiceClient::onVideoSendReportTick()
{
    if (!localVideo)
        return;
    const qint64 now = videoClock.nsecsElapsed() / 1000;
    const LocalVideoLayer *full = fullLayer();

    VideoSendStats stats;
    stats.codec = videoCodecName(sendCodec);
    stats.keyframeRequests = localKeyframeRequests;
    stats.receivers = full ? full->receivers : -1;
    stats.receiverEstimateBps = videoReceiverEstimateBps;
    stats.delayEstimateBps = static_cast<quint64>(sendEstimator.estimate());
    stats.acknowledgedBps = static_cast<quint64>(sendEstimator.acknowledged());
    reportedDelayEstimateBps = stats.delayEstimateBps;

    // What every layer sent, taken together.
    quint64 bytesSince = 0;
    for (LocalVideoLayer &layer : localLayers) {
        if (!layer.sender)
            continue;
        if (layer.sender->canReport() && currentState == State::Connected && encryption && udpTransport) {
            PacketPool::Handle packet = udpTransport->acquirePacket();
            packet->resize(packet->tailroom() - VoiceEncryption::TRAILER_SIZE);
            const int size = layer.sender->writeSenderReport(packet->bytes(), now, ntpTimeNow());
            if (size > 0 && packet->resize(size) && encryption->seal(*packet, RTCP_CLEAR_SIZE))
                udpTransport->send(*packet);
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
    if (now > videoSendReportedUs)
        stats.bitrateBps = bytesSince * 8 * 1000000 / static_cast<quint64>(now - videoSendReportedUs);
    videoSendReportedUs = now;

    emit videoSendStatsUpdated(stats);
}

void VoiceClient::receiveVideoFeedback(const ReceivedRtcp &received)
{
    if (localLayers.empty())
        return;

    for (const ReceivedRtcp::ReceiverCount &count : received.receiverCounts) {
        if (LocalVideoLayer *layer = localLayerOf(count.ssrc))
            noteVideoReceivers(*layer, count.receivers);
    }

    if (received.hasReceiverEstimate) {
        if (received.receiverEstimateBitsPerSecond != videoReceiverEstimateBps)
            qCDebug(LogVoice) << "Receiving side's estimate of the way from here:" << received.receiverEstimateBitsPerSecond << "bps";
        videoReceiverEstimateBps = received.receiverEstimateBitsPerSecond;
    }

    if (!localVideo)
        return;

    const qint64 now = videoClock.nsecsElapsed() / 1000;
    for (const ReceivedRtcp::Nack &nack : received.nacks) {
        if (LocalVideoLayer *layer = localLayerOf(nack.mediaSsrc); layer && layer->sender)
            layer->sender->retransmit(nack.sequences, now);
    }
    if (std::any_of(localLayers.cbegin(), localLayers.cend(), [](const LocalVideoLayer &layer) { return layer.sender && layer.sender->hasWaiting(); }))
        startVideoPacing();

    for (const ReceptionReport &report : received.receptionReports) {
        LocalVideoLayer *layer = localLayerOf(report.ssrc);
        if (!layer || !layer->sender)
            continue;
        layer->sender->onReceptionReport(report, now);
        if (layer->sender->stats().roundTripMs >= 0)
            sendEstimator.setRoundTrip(qint64(layer->sender->stats().roundTripMs) * 1000);
    }

    for (const ReceivedRtcp::TransportReport &report : received.transportReports) {
        if (sendEstimator.reportsTaken() == 0)
            qCInfo(LogVoice) << "The server reports when this client's video packets reach it";
        sendEstimator.onReport(report, now);
    }
    // Whoever sets the encoder's bitrate hears of it with the statistics, which a way that
    // has just been found to fall behind is not left to wait for.
    const quint64 estimate = static_cast<quint64>(sendEstimator.estimate());
    if (estimate > 0 && (reportedDelayEstimateBps == 0 || estimate < reportedDelayEstimateBps * DELAY_ESTIMATE_STEP) && videoSendReportTimer && videoSendReportTimer->isActive()) {
        qCDebug(LogVoice) << "The way from here is falling behind: it is taken to carry" << estimate << "bps of the" << sendEstimator.acknowledged() << "the server acknowledges";
        onVideoSendReportTick();
        videoSendReportTimer->start(VIDEO_SEND_REPORT_INTERVAL_MS);
    }

    for (const uint32_t ssrc : received.keyframeRequests) {
        LocalVideoLayer *layer = localLayerOf(ssrc);
        if (!layer)
            continue;
        localKeyframeRequests++;
        askLayerForKeyframe(*layer);
    }
}

// The official client tears down and rebuilds every video receiver it has for the sender, which
// leaves each waiting for a key frame and asking for one. Starting each over does the same.
void VoiceClient::receiveVideoAfresh(quint32 ssrc)
{
    Core::Snowflake sender;
    if (const auto known = ssrcToUserIdMap.constFind(ssrc); known != ssrcToUserIdMap.constEnd())
        sender = Core::Snowflake(known.value());
    for (const auto &[videoSsrc, video] : remoteVideos) {
        if (videoSsrc == ssrc || video.rtxSsrc == ssrc)
            sender = video.userId;
    }
    qCDebug(LogVoice) << "Server asks for the video of SSRC" << ssrc << "to be received afresh";
    if (!sender.isValid())
        return;

    for (auto &[videoSsrc, video] : remoteVideos) {
        if (video.receiver && video.userId == sender)
            video.receiver->requestKeyframe();
    }
}

void VoiceClient::noteVideoReceivers(LocalVideoLayer &layer, int receivers)
{
    if (receivers == layer.receivers)
        return;

    // Whoever has only now begun to receive has seen none of what was sent so far.
    const bool firstReceiver = layer.receivers <= 0 && receivers > 0;
    layer.receivers = receivers;
    emit videoReceiversChanged(receivers, layer.quality);
    if (firstReceiver)
        askLayerForKeyframe(layer);
}

void VoiceClient::askForLocalKeyframe()
{
    for (LocalVideoLayer &layer : localLayers)
        askLayerForKeyframe(layer);
}

void VoiceClient::askForAwaitedLocalKeyframes()
{
    for (LocalVideoLayer &layer : localLayers) {
        if (layer.awaitsKeyframe)
            askLayerForKeyframe(layer);
    }
}

void VoiceClient::askForWaitingLocalKeyframes()
{
    for (LocalVideoLayer &layer : localLayers) {
        if (layer.keyframeAskWaiting)
            askLayerForKeyframe(layer);
    }
}

void VoiceClient::askLayerForKeyframe(LocalVideoLayer &layer)
{
    if (!localVideo)
        return;

    const qint64 now = videoClock.elapsed();
    const qint64 wait = layer.keyframeAskedMs < 0 ? 0 : layer.keyframeAskedMs + LOCAL_KEYFRAME_INTERVAL_MS - now;
    if (wait > 0) {
        // Too soon after the last one: this request is passed on when the interval is over,
        // together with any others that come until then.
        layer.keyframeAskWaiting = true;
        if (!localKeyframeTimer) {
            localKeyframeTimer = new QTimer(this);
            localKeyframeTimer->setSingleShot(true);
            connect(localKeyframeTimer, &QTimer::timeout, this, &VoiceClient::askForWaitingLocalKeyframes);
        }
        if (!localKeyframeTimer->isActive())
            localKeyframeTimer->start(static_cast<int>(wait));
        return;
    }

    layer.keyframeAskWaiting = false;
    layer.keyframeAskedMs = now;
    emit videoKeyframeRequested(layer.quality);
}

void VoiceClient::onSessionUpdated(const SessionUpdate &update)
{
    if (update.videoCodec.hasValue())
        setVideoSendCodec(update.videoCodec.get());
}

// What is said of a layer's own SSRC counts before what is said of any, and when neither
// says anything all of it is wanted: a want of nothing is read as a want of everything, as
// the official client reads it.
void VoiceClient::onMediaSinkWants(const MediaSinkWants &wants)
{
    for (const LocalVideoLayer &layer : localLayers) {
        int want = 100;
        if (const auto own = wants.bySsrc.constFind(layer.ssrc); own != wants.bySsrc.constEnd() && own.value() > 0)
            want = own.value();
        else if (wants.any && *wants.any > 0)
            want = *wants.any;

        const auto pixelCount = wants.pixelCounts.constFind(layer.ssrc);
        emit videoWantsReceived(want, pixelCount != wants.pixelCounts.constEnd() ? pixelCount.value() : 0, layer.quality);
    }
}

void VoiceClient::sendVideoWants()
{
    if (currentState != State::Connected || !gateway || videoPayloadTypes.isEmpty())
        return;
    // A stream's viewer has nothing to say until the broadcaster's video is announced, and a
    // voice connection nothing until it has been given a plan.
    if (!explicitVideoPlan && (!params.streamContext || remoteLayers.isEmpty()))
        return;

    const QByteArray serialized = plannedWants.toJson().toBytes();
    if (serialized == sentVideoWants)
        return;
    sentVideoWants = serialized;
    gateway->sendMediaSinkWants(plannedWants);
}

void VoiceClient::requestVideoKeyframeFrom(Core::Snowflake userId)
{
    for (auto &[ssrc, video] : remoteVideos) {
        if (video.receiver && video.userId == userId)
            video.receiver->requestKeyframe();
    }
}

void VoiceClient::requestVideoKeyframeFromEveryone()
{
    for (auto &[ssrc, video] : remoteVideos) {
        if (video.receiver)
            video.receiver->requestKeyframe();
    }
}

void VoiceClient::withdrawVideoCodec(VideoCodec codec)
{
    bool changed = false;
    for (Codec &offered : params.videoCodecs) {
        if (videoCodecFromName(offered.name) != codec || !offered.decode.valueOr(false))
            continue;
        offered.decode = false;
        changed = true;
    }
    if (!changed || !gateway || currentState != State::Connected)
        return;

    qCWarning(LogVoice) << "Withdrawing" << videoCodecName(codec) << "from the codecs this client decodes";
    sendOfferedCodecs();
}

void VoiceClient::startVideoTimer()
{
    if (!videoClock.isValid())
        videoClock.start();
    videoReportedMs = videoClock.elapsed();

    if (!videoTimer) {
        videoTimer = new QTimer(this);
        videoTimer->setTimerType(Qt::PreciseTimer);
        connect(videoTimer, &QTimer::timeout, this, &VoiceClient::onVideoTick);
    }
    videoTimer->start(VIDEO_TICK_MS);
}

void VoiceClient::onVideoTick()
{
    if (currentState != State::Connected || !encryption || !udpTransport)
        return;

    const qint64 now = videoClock.elapsed();
    const bool reportDue = now - videoReportedMs >= VIDEO_REPORT_INTERVAL_MS;

    struct Pending
    {
        quint32 ssrc;
        VideoReceiver::Feedback feedback;
    };
    std::vector<Pending> pending;
    std::vector<ReceptionReport> reports;
    VideoReceiveStats stats;

    for (auto &[ssrc, video] : remoteVideos) {
        if (!video.receiver)
            continue;

        VideoReceiver::Feedback feedback = video.receiver->poll(now);
        if (!feedback.nacks.empty() || feedback.keyframeRequest)
            pending.push_back({ ssrc, std::move(feedback) });

        if (!reportDue)
            continue;
        ReceptionReport report = video.receiver->receptionReport();
        if (const auto heard = senderReports.constFind(ssrc); heard != senderReports.constEnd()) {
            // The delay is in units of 1/65536 of a second.
            report.lastSenderReport = heard->compactNtp;
            report.delaySinceLastSenderReport = static_cast<quint32>((videoClock.nsecsElapsed() / 1000 - heard->arrivalUs) * 65536 / 1000000);
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

    const bool transportDue = transportFeedback.hasPending() && now - transportReportedMs >= TRANSPORT_FEEDBACK_INTERVAL_MS;
    if (pending.empty() && reports.empty() && !transportDue) {
        if (reportDue)
            videoReportedMs = now;
        return;
    }

    // A compound packet opens with a report, so feedback sent between reports follows an empty one.
    PacketPool::Handle packet = udpTransport->acquirePacket();
    packet->resize(packet->tailroom() - VoiceEncryption::TRAILER_SIZE);
    int size = writeReceiverReport(packet->bytes(), localSsrc, reports);
    if (size < 0)
        return;
    if (transportDue) {
        transportReportedMs = now;
        // One packet covers a bounded span; after a long gap it takes more than one.
        for (int i = 0; i < 4 && transportFeedback.hasPending(); i++) {
            const int written = transportFeedback.write(packet->bytes().subspan(static_cast<size_t>(size)), localSsrc, transportMediaSsrc);
            if (written <= 0)
                break;
            size += written;
        }
    }

    for (const Pending &entry : pending) {
        if (!entry.feedback.nacks.empty()) {
            const int written = writeNack(packet->bytes().subspan(static_cast<size_t>(size)), localSsrc, entry.ssrc, entry.feedback.nacks);
            if (written > 0)
                size += written;
        }
        if (entry.feedback.keyframeRequest) {
            const int written = writePli(packet->bytes().subspan(static_cast<size_t>(size)), localSsrc, entry.ssrc);
            if (written > 0)
                size += written;
        }
    }
    packet->resize(size);

    if (encryption->seal(*packet, RTCP_CLEAR_SIZE))
        udpTransport->send(*packet);

    if (reportDue) {
        videoReportedMs = now;
        stats.decryptFailures = videoDecryptFailures;
        stats.serverEstimateBps = serverEstimateBps;
        stats.serverLossFraction = serverLossFraction;
        if (!remoteVideos.empty())
            emit videoStatsUpdated(stats);
    }
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
