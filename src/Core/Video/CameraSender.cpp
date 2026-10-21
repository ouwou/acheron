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
using Video::CAMERA_FULL_LAYER;
using Video::CAMERA_SECOND_LAYER;

CameraSender::CameraSender(Discord::Client *client, Audio::VoiceManager *voice, StreamBroadcaster *screenShare, Permission allowedHere, QObject *parent)
    : QObject(parent), client(client), voice(voice), screenShare(screenShare), allowedHere(std::move(allowedHere))
{
    clock.start();
    layers[CAMERA_SECOND_LAYER].quality = Discord::Voice::SECOND_LAYER_QUALITY;
    layers[CAMERA_SECOND_LAYER].suspended = true;

    planTimer.setSingleShot(true);
    connect(&planTimer, &QTimer::timeout, this, &CameraSender::applyPlan);

    codecTimeout.setSingleShot(true);
    codecTimeout.setInterval(Video::SEND_CODEC_TIMEOUT_MS);
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
        fail(tr("Your camera was turned off because the connection to Discord was restarted."), GatewayNotice::Skip);
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
    Stats now;
    now.network = networkStats;
    now.mutedFramerate = pipeline && layers[CAMERA_FULL_LAYER].plan.countsAsMuted(clock.elapsed());
    now.hasSecondLayer = hasSecondLayer;
    for (size_t index = 0; index < layers.size(); index++) {
        const Layer &layer = layers[index];
        now.layers[index] = { layer.encoderStats, layer.asked.bitrateBps, layer.receivers, layer.plan.servedWant(), layer.suspended };
    }
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

    sendCodecs.ask(this, [this](const QList<VideoCodec> &codecs) { onEncodersKnown(codecs); });
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
    end(GatewayNotice::Send, State::Off);
}

void CameraSender::end(GatewayNotice notice, State endState)
{
    if (!isOn())
        return;

    sendCodecs.cancel();
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

    // when the server moved the account, its voice state in the new channel still shows the camera
    const Snowflake here = voice->currentChannelId();
    if (notice == GatewayNotice::Send && here.isValid() && (here == left || voice->isSelfVideo()))
        sendVoiceState();

    emit stateChanged();
}

void CameraSender::fail(const QString &reason, GatewayNotice notice)
{
    qCWarning(LogVoice) << "Camera turned off:" << reason;
    failureText = reason;
    if (isOn())
        end(notice, State::Failed);
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

// a voice state update naming the channel would rejoin it
void CameraSender::onVoiceDisconnected()
{
    speaking = false;
    end(GatewayNotice::Skip, State::Off);
    clearFailure();
}

void CameraSender::onVoiceMembershipChanged()
{
    if (isOn() && voice->currentChannelId() != cameraChannelId)
        end(GatewayNotice::Send, State::Off);
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
        layer.receivers.reset();
    codecTimeout.stop();

    if (currentState == State::On)
        setState(State::Starting);
    applySuspension();
}

void CameraSender::onRtcStateChanged(VoiceClient::State state)
{
    rtcConnected = state == VoiceClient::State::Connected;
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

    // as WebRTC splits a rate among a stream's layers
    Layer &full = layers[CAMERA_FULL_LAYER];
    Layer &second = layers[CAMERA_SECOND_LAYER];
    const std::optional<qint64> ceiling = Video::estimatedCeiling(sent.receiverEstimateBps, sent.delayEstimateBps);
    const bool sharedWithSecond = ceiling && hasSecondLayer && !second.suspended;
    second.plan.setReceiverLimit(ceiling);
    full.plan.setReceiverLimit(sharedWithSecond ? std::optional<qint64>(std::max<qint64>(*ceiling - second.asked.bitrateBps, 1)) : ceiling);
    for (Layer &layer : layers)
        layer.plan.reportLoss(sent.lossFraction, clock.elapsed());
    applyPlan();
}

void CameraSender::onSendLayersKnown(const QList<int> &qualities)
{
    hasSecondLayer = qualities.contains(layers[CAMERA_SECOND_LAYER].quality);
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
    // the pipeline is the context, so whatever it still had on the way goes with it
    Video::CameraPipeline *started = pipeline.get();
    connect(started, &Video::CameraPipeline::started, started, [this](const QSize &size, int, int layer) {
        if (layer == CAMERA_FULL_LAYER)
            onPipelineStarted(size);
    });
    connect(started, &Video::CameraPipeline::previewReady, started, [this](const QImage &preview) { onPreviewReady(preview); });
    connect(started, &Video::CameraPipeline::failed, started, [this](const QString &reason) { onPipelineFailed(reason); });
    connect(started, &Video::CameraPipeline::statsUpdated, started, [this](const Video::PipelineStats &encoder, int layer) { onPipelineStats(encoder, layer); });
    sendFramesToRtc();

    for (Layer &layer : layers)
        layer.asked = layer.plan.advance(clock.elapsed());
    const Layer &full = layers[CAMERA_FULL_LAYER];
    const Layer &second = layers[CAMERA_SECOND_LAYER];
    Video::CameraSettings settings;
    settings.deviceId = deviceId;
    settings.codec = sendCodec;
    settings.encodeSize = full.asked.size;
    settings.framerate = full.asked.framerate;
    settings.bitrateBps = full.asked.bitrateBps;
    settings.maxBitrateBps = Video::CAMERA_MAX_BITRATE;
    pipeline->setEncoding(second.asked.size, second.asked.framerate, CAMERA_SECOND_LAYER);
    pipeline->setBitrate(second.asked.bitrateBps, CAMERA_SECOND_LAYER);
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
        layer.suspended = &layer == &layers[CAMERA_SECOND_LAYER];
        layer.encoderStats = {};
    }
}

void CameraSender::sendFramesToRtc()
{
    QObject::disconnect(framesToRtc);
    if (!pipeline || !rtc)
        return;

    VoiceClient *client = rtc->client();
    framesToRtc = connect(pipeline.get(), &Video::CameraPipeline::frameEncoded, client, [connection = client, secondQuality = layers[CAMERA_SECOND_LAYER].quality](const Video::EncodedFramePointer &frame, int layer) {
        connection->sendVideoFrame(*frame, layer == CAMERA_SECOND_LAYER ? secondQuality : Discord::Voice::FULL_QUALITY);
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

void CameraSender::onPipelineStats(const Video::PipelineStats &encoder, int layer)
{
    if (layer == CAMERA_FULL_LAYER || layer == CAMERA_SECOND_LAYER)
        layers[static_cast<size_t>(layer)].encoderStats = encoder;
}

void CameraSender::announceVideo()
{
    // as the official client
    LocalVideoDescription description;
    description.maxSize = Video::CAMERA_CAPTURE_SIZE;
    description.maxFramerate = Video::CAMERA_FRAMERATE;
    description.maxBitrateBps = Video::CAMERA_MAX_BITRATE;
    description.secondLayerMaxBitrateBps = layers[CAMERA_SECOND_LAYER].plan.maxBitrate();

    videoAnnounced = true;
    rtc->run([description, bitrate = layers[CAMERA_FULL_LAYER].asked.bitrateBps, secondBitrate = layers[CAMERA_SECOND_LAYER].asked.bitrateBps, secondQuality = layers[CAMERA_SECOND_LAYER].quality](VoiceClient *connection) {
        connection->startVideo(description);
        connection->setVideoBitrate(bitrate);
        connection->setVideoBitrate(secondBitrate, secondQuality);
    });
    applySuspension();
    // the client dropped what came before it took the announcement, including the first key frame
    pipeline->requestKeyframe(CAMERA_FULL_LAYER);
    pipeline->requestKeyframe(CAMERA_SECOND_LAYER);
    setState(State::On);
}

// the official client announces none of these
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
        if (wanted.bitrateBps != layer.asked.bitrateBps) {
            pipeline->setBitrate(wanted.bitrateBps, index);
            if (rtc)
                rtc->run([bitrate = wanted.bitrateBps, quality = layer.quality](VoiceClient *connection) { connection->setVideoBitrate(bitrate, quality); });
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

void CameraSender::applySuspension()
{
    if (!pipeline)
        return;

    Layer &full = layers[CAMERA_FULL_LAYER];
    Layer &second = layers[CAMERA_SECOND_LAYER];
    const bool suspendFull = Video::encodingSuspends(pipelineStarted, videoAnnounced, full.receivers);
    const bool suspendSecond = !hasSecondLayer || !pipelineStarted || Video::encodingSuspends(pipelineStarted, videoAnnounced, second.receivers);
    if (suspendFull != std::exchange(full.suspended, suspendFull))
        pipeline->setEncodingSuspended(suspendFull, CAMERA_FULL_LAYER);
    if (suspendSecond != std::exchange(second.suspended, suspendSecond))
        pipeline->setEncodingSuspended(suspendSecond, CAMERA_SECOND_LAYER);
}

} // namespace Core
} // namespace Acheron
