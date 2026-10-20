#include "Core/Video/CameraSender.hpp"

#include "Core/Audio/VoiceConnection.hpp"
#include "Core/Audio/VoiceManager.hpp"
#include "Core/Logging.hpp"
#include "Core/Media/CodecSupport.hpp"
#include "Core/Streams/StreamBroadcaster.hpp"
#include "Core/Video/CameraPipeline.hpp"
#include "Discord/Client.hpp"

#include <optional>
#include <utility>

namespace Acheron {
namespace Core {

using Discord::Voice::LocalVideoDescription;
using Discord::Voice::VideoCodec;
using Discord::Voice::VideoSendStats;
using Discord::Voice::VoiceClient;

CameraSender::CameraSender(Discord::Client *client, Audio::VoiceManager *voice, StreamBroadcaster *screenShare, Permission allowedHere, QObject *parent)
    : QObject(parent), client(client), voice(voice), screenShare(screenShare), allowedHere(std::move(allowedHere))
{
    clock.start();
    layers[SECOND].quality = Video::CAMERA_SECOND_QUALITY;
    layers[SECOND].suspended = true;

    planTimer.setSingleShot(true);
    connect(&planTimer, &QTimer::timeout, this, &CameraSender::applyPlan);

    codecTimeout.setSingleShot(true);
    codecTimeout.setInterval(CODEC_TIMEOUT_MS);
    connect(&codecTimeout, &QTimer::timeout, this, &CameraSender::onCodecTimedOut);

    connect(voice, &Audio::VoiceManager::rtcConnectionCreated, this, &CameraSender::onRtcConnectionCreated);
    connect(voice, &Audio::VoiceManager::rtcConnectionClosing, this, &CameraSender::onRtcConnectionClosing);
    connect(voice, &Audio::VoiceManager::voiceDisconnected, this, &CameraSender::onVoiceDisconnected);
    connect(voice, &Audio::VoiceManager::channelVoiceMemberChanged, this, &CameraSender::onVoiceMembershipChanged);
    connect(voice, &Audio::VoiceManager::participantVoiceStateChanged, this, &CameraSender::onOwnVoiceStateChanged);
    connect(voice, &Audio::VoiceManager::speakingChanged, this, &CameraSender::onSpeakingChanged);
    connect(screenShare, &StreamBroadcaster::stateChanged, this, &CameraSender::onScreenShareChanged);

    if (voice->rtcConnection()) {
        onRtcConnectionCreated(voice->rtcConnection());
        rtcConnected = voice->isConnected();
    }
}

CameraSender::~CameraSender()
{
    QObject::disconnect(framesToRtc);
    stopPipeline();
}

bool CameraSender::canTurnOn() const
{
    return Video::cameraCaptureAvailable() && voice->isConnected() && allowedHere();
}

void CameraSender::reset()
{
    if (isOn())
        fail(tr("Your camera was turned off because the connection to Discord was restarted."), false);
}

void CameraSender::setState(State state)
{
    if (currentState == state)
        return;
    currentState = state;
    emit stateChanged();
}

void CameraSender::clearFailure()
{
    if (currentState != State::Failed)
        return;
    failureText.clear();
    setState(State::Off);
}

CameraSender::Stats CameraSender::stats() const
{
    const Layer &full = layers[FULL];
    const Layer &second = layers[SECOND];

    Stats now;
    now.encoder = full.encoderStats;
    now.network = networkStats;
    now.targetBitrate = full.asked.bitrate;
    now.receivers = full.receivers;
    now.servedWant = full.plan.servedWant();
    now.mutedFramerate = pipeline && full.plan.countsAsMuted(clock.elapsed());
    now.encodingSuspended = full.suspended;
    now.hasSecondLayer = hasSecondLayer;
    now.secondEncoder = second.encoderStats;
    now.secondTargetBitrate = second.asked.bitrate;
    now.secondReceivers = second.receivers;
    now.secondServedWant = second.plan.servedWant();
    now.secondEncodingSuspended = second.suspended;
    return now;
}

void CameraSender::turnOn(const QString &wantedDeviceId)
{
    if (isOn()) {
        if (wantedDeviceId == deviceId)
            return;
        deviceId = wantedDeviceId;
        if (pipeline)
            startPipeline();
        return;
    }

    failureText.clear();
    if (!canTurnOn()) {
        fail(tr("You cannot turn on your camera here."));
        return;
    }

    deviceId = wantedDeviceId;
    cameraChannelId = voice->currentChannelId();
    for (Layer &layer : layers) {
        layer.plan.start(clock.elapsed());
        layer.plan.setSelfMuted(voice->isSelfMuted());
        layer.plan.setSpeaking(speaking, clock.elapsed());
        layer.plan.setSharingScreen(screenShare->isSharing());
    }
    setState(State::Starting);
    sendVoiceState();

    Media::whenSendCodecsKnown(this, [this, asked = ++encoderProbeAttempt](const QList<VideoCodec> &codecs) {
        if (asked == encoderProbeAttempt)
            onEncodersKnown(codecs);
    });
}

void CameraSender::onEncodersKnown(const QList<VideoCodec> &codecs)
{
    if (codecs.isEmpty()) {
        fail(tr("This machine has no video encoder that Discord accepts, so it cannot send a camera."));
        return;
    }

    encodersKnown = true;
    voice->setEncodableVideoCodecs(Media::cameraCodecs(codecs));
    sendOnceReady();
}

void CameraSender::turnOff()
{
    end(true, State::Off);
}

void CameraSender::end(bool tellGateway, State endState)
{
    if (!isOn())
        return;

    encoderProbeAttempt++;
    encodersKnown = false;
    codecTimeout.stop();
    planTimer.stop();

    if (rtc)
        rtc->run([](VoiceClient *connection) { connection->stopVideo(); });
    videoAnnounced = false;
    stopPipeline();
    latestPreview = QImage();
    networkStats = {};

    const Snowflake left = std::exchange(cameraChannelId, Snowflake::Invalid);
    currentState = endState;

    // Moved to another channel, the account's voice state there still shows the camera when
    // the server did the moving.
    const Snowflake here = voice->currentChannelId();
    if (tellGateway && here.isValid() && (here == left || voice->isSelfVideo()))
        sendVoiceState();

    emit stateChanged();
}

void CameraSender::fail(const QString &reason, bool tellGateway)
{
    qCWarning(LogVoice) << "Camera turned off:" << reason;
    failureText = reason;
    if (isOn())
        end(tellGateway, State::Failed);
    else
        setState(State::Failed);
    emit failed(reason);
}

void CameraSender::sendVoiceState()
{
    const Snowflake channelId = voice->currentChannelId();
    client->sendVoiceStateUpdate(voice->currentGuildId(), channelId, voice->isSelfMuted(), voice->isSelfDeafened(), isOnIn(channelId));
}

void CameraSender::onCodecTimedOut()
{
    fail(tr("Your camera could not be turned on: Discord named no video codec this machine can encode."));
}

// The call is over, and with it whatever was said of the camera in it. Nothing is sent to the
// gateway: a voice state update that names the channel would join it again.
void CameraSender::onVoiceDisconnected()
{
    speaking = false;
    end(false, State::Off);
    clearFailure();
}

void CameraSender::onVoiceMembershipChanged()
{
    if (isOn() && voice->currentChannelId() != cameraChannelId)
        end(true, State::Off);
}

void CameraSender::onOwnVoiceStateChanged()
{
    for (Layer &layer : layers)
        layer.plan.setSelfMuted(voice->isSelfMuted());
    applyPlan();
}

void CameraSender::onSpeakingChanged(bool nowSpeaking)
{
    speaking = nowSpeaking;
    for (Layer &layer : layers)
        layer.plan.setSpeaking(nowSpeaking, clock.elapsed());
    applyPlan();
}

void CameraSender::onScreenShareChanged()
{
    for (Layer &layer : layers)
        layer.plan.setSharingScreen(screenShare->isSharing());
    applyPlan();
}

void CameraSender::onRtcConnectionCreated(Audio::VoiceConnection *connection)
{
    onRtcConnectionClosing();

    rtc = connection;
    connect(rtc, &Audio::VoiceConnection::stateChanged, this, &CameraSender::onRtcStateChanged);
    connect(rtc, &Audio::VoiceConnection::videoSendCodecChanged, this, &CameraSender::onSendCodecChanged);
    connect(rtc, &Audio::VoiceConnection::videoSendStatsUpdated, this, &CameraSender::onSendStats);
    connect(rtc, &Audio::VoiceConnection::videoSendLayersKnown, this, &CameraSender::onSendLayersKnown);
    connect(rtc, &Audio::VoiceConnection::videoKeyframeRequested, this, &CameraSender::onKeyframeRequested);
    connect(rtc, &Audio::VoiceConnection::videoReceiversChanged, this, &CameraSender::onReceiversChanged);
    connect(rtc, &Audio::VoiceConnection::videoWantsReceived, this, [this](int want, int, int quality) { onWantsReceived(want, quality); });
    sendFramesToRtc();
}

void CameraSender::onRtcConnectionClosing()
{
    if (!rtc)
        return;

    QObject::disconnect(framesToRtc);
    QObject::disconnect(rtc, nullptr, this, nullptr);
    rtc = nullptr;
    rtcConnected = false;
    videoAnnounced = false;
    sendCodec = VideoCodec::Unknown;
    hasSecondLayer = false;
    for (Layer &layer : layers)
        layer.receivers = -1;
    codecTimeout.stop();

    if (currentState == State::On)
        setState(State::Starting);
    applySuspension();
}

void CameraSender::onRtcStateChanged(VoiceClient::State state)
{
    rtcConnected = state == VoiceClient::State::Connected;
    // A connection that had to be made again has forgotten what it was told of the camera.
    videoAnnounced = false;
    if (rtcConnected) {
        sendOnceReady();
        return;
    }

    codecTimeout.stop();
    if (currentState == State::On)
        setState(State::Starting);
    applySuspension();
}

void CameraSender::onSendCodecChanged(VideoCodec codec)
{
    sendCodec = codec;
    sendOnceReady();
}

void CameraSender::onSendStats(const VideoSendStats &sent)
{
    networkStats = sent;
    if (!pipelineStarted)
        return;

    // What the way carries goes to the smaller layer first and the rest to the full one, as
    // WebRTC shares a rate out among the layers of a stream.
    Layer &full = layers[FULL];
    Layer &second = layers[SECOND];
    const qint64 ceiling = Video::estimatedCeiling(sent.receiverEstimateBps, sent.delayEstimateBps);
    const bool sharedWithSecond = ceiling > 0 && hasSecondLayer && !second.suspended;
    second.plan.setReceiverLimit(ceiling);
    full.plan.setReceiverLimit(sharedWithSecond ? std::max<qint64>(ceiling - second.asked.bitrate, 1) : ceiling);
    for (Layer &layer : layers)
        layer.plan.reportLoss(sent.lossFraction, clock.elapsed());
    applyPlan();
}

void CameraSender::onSendLayersKnown(const QList<int> &qualities)
{
    hasSecondLayer = qualities.contains(layers[SECOND].quality);
    applySuspension();
}

void CameraSender::onKeyframeRequested(int quality)
{
    if (pipeline)
        pipeline->requestKeyframe(layerOf(quality));
}

void CameraSender::onReceiversChanged(int count, int quality)
{
    layers[static_cast<size_t>(layerOf(quality))].receivers = count;
    applySuspension();
}

void CameraSender::onWantsReceived(int want, int quality)
{
    layers[static_cast<size_t>(layerOf(quality))].plan.setWant(want);
    applyPlan();
}

void CameraSender::sendOnceReady()
{
    if (!isOn() || !encodersKnown || !rtcConnected)
        return;

    if (sendCodec == VideoCodec::Unknown) {
        // A connection made before the encoders were known hears of them late, and the server
        // then names the codec to use. One it never names cannot be waited for forever.
        if (!codecTimeout.isActive())
            codecTimeout.start();
        return;
    }
    codecTimeout.stop();

    if (!pipeline || pipelineCodec != sendCodec)
        startPipeline();
    else if (pipelineStarted && !videoAnnounced)
        announceVideo();
}

void CameraSender::startPipeline()
{
    stopPipeline();

    pipeline = std::make_unique<Video::CameraPipeline>();
    pipelineCodec = sendCodec;
    // In the pipeline's own context, so that what it had on its way when it was let go of
    // goes with it.
    Video::CameraPipeline *started = pipeline.get();
    connect(started, &Video::CameraPipeline::started, started, [this](const QSize &size, int, int layer) {
        if (layer == FULL)
            onPipelineStarted(size);
    });
    connect(started, &Video::CameraPipeline::previewReady, started, [this](const QImage &preview) { onPreviewReady(preview); });
    connect(started, &Video::CameraPipeline::failed, started, [this](const QString &reason) { onPipelineFailed(reason); });
    connect(started, &Video::CameraPipeline::statsUpdated, started, [this](const Video::BroadcastStats &encoder, int layer) { onPipelineStats(encoder, layer); });
    sendFramesToRtc();

    for (Layer &layer : layers)
        layer.asked = layer.plan.advance(clock.elapsed());
    const Layer &full = layers[FULL];
    const Layer &second = layers[SECOND];
    Video::CameraSettings settings;
    settings.deviceId = deviceId;
    settings.codec = sendCodec;
    settings.encodeSize = full.asked.size;
    settings.framerate = full.asked.framerate;
    settings.bitrate = full.asked.bitrate;
    // The most the camera is ever sent at, whichever row of the ladder it starts on.
    settings.maxBitrate = Video::CAMERA_MAX_BITRATE;
    // The second layer is encoded once someone is known to receive it.
    pipeline->setEncoding(second.asked.size, second.asked.framerate, SECOND);
    pipeline->setBitrate(second.asked.bitrate, SECOND);
    pipeline->start(settings);
    applyPlan();
}

void CameraSender::stopPipeline()
{
    if (!pipeline)
        return;

    planTimer.stop();
    QObject::disconnect(framesToRtc);
    pipeline->stop();
    pipeline.reset();

    pipelineStarted = false;
    for (Layer &layer : layers) {
        layer.suspended = &layer == &layers[SECOND];
        layer.encoderStats = {};
    }
}

void CameraSender::sendFramesToRtc()
{
    QObject::disconnect(framesToRtc);
    if (!pipeline || !rtc)
        return;

    VoiceClient *client = rtc->client();
    framesToRtc = connect(pipeline.get(), &Video::CameraPipeline::frameEncoded, client, [connection = client, secondQuality = layers[SECOND].quality](const Video::EncodedFramePointer &frame, int layer) {
        connection->sendVideoFrame(*frame, layer == SECOND ? secondQuality : VoiceClient::FULL_QUALITY);
    });
}

void CameraSender::onPipelineStarted(const QSize &encodedSize)
{
    qCInfo(LogVoice) << "Camera is encoding at" << encodedSize;
    pipelineStarted = true;
    sendOnceReady();
    applySuspension();
}

void CameraSender::onPreviewReady(const QImage &preview)
{
    const bool first = latestPreview.isNull();
    latestPreview = preview;
    emit previewChanged(preview);
    if (first)
        emit stateChanged();
}

void CameraSender::onPipelineFailed(const QString &reason)
{
    fail(tr("Your camera was turned off: %1").arg(reason));
}

void CameraSender::onPipelineStats(const Video::BroadcastStats &encoder, int layer)
{
    if (layer == FULL || layer == SECOND)
        layers[static_cast<size_t>(layer)].encoderStats = encoder;
}

void CameraSender::announceVideo()
{
    // Both layers are announced at the camera's own size, as the official client announces
    // them, and each at the most its place on the ladder is sent at.
    LocalVideoDescription description;
    description.maxWidth = Video::CAMERA_CAPTURE_SIZE.width();
    description.maxHeight = Video::CAMERA_CAPTURE_SIZE.height();
    description.maxFramerate = Video::CAMERA_FRAMERATE;
    description.maxBitrate = Video::CAMERA_MAX_BITRATE;
    description.secondLayerMaxBitrate = layers[SECOND].plan.maxBitrate();

    videoAnnounced = true;
    rtc->run([description, bitrate = layers[FULL].asked.bitrate, secondBitrate = layers[SECOND].asked.bitrate, secondQuality = layers[SECOND].quality](VoiceClient *connection) {
        connection->startVideo(description);
        connection->setVideoBitrate(bitrate);
        connection->setVideoBitrate(secondBitrate, secondQuality);
    });
    applySuspension();
    // What was encoded before the client took the announcement was dropped by it, and that
    // included the frame every viewer needs first.
    pipeline->requestKeyframe(FULL);
    pipeline->requestKeyframe(SECOND);
    setState(State::On);
}

// The size, frame rate and bitrate change what is encoded and nothing else: the official
// client announces none of them.
void CameraSender::applyPlan()
{
    planTimer.stop();
    if (!pipeline)
        return;

    const qint64 now = clock.elapsed();
    std::optional<qint64> nextChange;
    for (int index = 0; index < static_cast<int>(layers.size()); index++) {
        Layer &layer = layers[static_cast<size_t>(index)];
        const Video::CameraEncoding wanted = layer.plan.advance(now);
        // The bitrate first, so that an encoder reopened for a new size opens at the rate that
        // goes with it.
        if (wanted.bitrate != layer.asked.bitrate) {
            pipeline->setBitrate(wanted.bitrate, index);
            if (rtc)
                rtc->run([bitrate = wanted.bitrate, quality = layer.quality](VoiceClient *connection) { connection->setVideoBitrate(bitrate, quality); });
        }
        if (wanted.size != layer.asked.size || wanted.framerate != layer.asked.framerate)
            pipeline->setEncoding(wanted.size, wanted.framerate, index);
        layer.asked = wanted;

        if (const std::optional<qint64> wait = layer.plan.nextChangeIn(now))
            nextChange = nextChange ? std::min(*nextChange, *wait) : *wait;
    }
    if (nextChange)
        planTimer.start(static_cast<int>(*nextChange));
}

// Nothing is encoded while nobody is there to receive it, as in the official client; the
// camera stays open and its preview live. The full layer only once the pipeline has started,
// though: it is not promised to report a start while it encodes nothing. The second layer
// is encoded only on a connection that has one, once the video has been announced.
void CameraSender::applySuspension()
{
    if (!pipeline)
        return;

    Layer &full = layers[FULL];
    Layer &second = layers[SECOND];
    const bool suspendFull = pipelineStarted && (!videoAnnounced || full.receivers == 0);
    const bool suspendSecond = !hasSecondLayer || !pipelineStarted || !videoAnnounced || second.receivers == 0;
    if (suspendFull != std::exchange(full.suspended, suspendFull))
        pipeline->setEncodingSuspended(suspendFull, FULL);
    if (suspendSecond != std::exchange(second.suspended, suspendSecond))
        pipeline->setEncodingSuspended(suspendSecond, SECOND);
}

} // namespace Core
} // namespace Acheron
