#include "StreamBroadcaster.hpp"

#include "Core/Audio/LoopbackCapture.hpp"
#include "Core/Audio/VoiceConnection.hpp"
#include "Core/Audio/VoiceManager.hpp"
#include "Core/Logging.hpp"
#include "Core/Media/CodecSupport.hpp"
#include "Core/Streams/StreamAudio.hpp"
#include "Core/Streams/StreamPreviews.hpp"
#include "Core/Video/ScreenCapture.hpp"
#include "Discord/Client.hpp"

#include <QCoreApplication>

#include <algorithm>
#include <utility>

namespace Acheron {
namespace Core {

using Discord::Voice::LocalVideoDescription;
using Discord::Voice::VideoCodec;
using Discord::Voice::VideoSendStats;
using Discord::Voice::VoiceClient;

namespace {

bool sameDescription(const LocalVideoDescription &a, const LocalVideoDescription &b)
{
    return a.maxWidth == b.maxWidth && a.maxHeight == b.maxHeight && a.maxFramerate == b.maxFramerate && a.maxBitrate == b.maxBitrate;
}

} // namespace

StreamBroadcaster::StreamBroadcaster(Discord::Client *client, StreamManager *streams, Audio::VoiceManager *voice, Snowflake accountId, const ProxyConfig &proxy, TermsProvider terms, QObject *parent)
    : QObject(parent), client(client), streams(streams), voice(voice), accountId(accountId), proxy(proxy), terms(std::move(terms))
{
    rateClock.start();

    startTimeout.setSingleShot(true);
    startTimeout.setInterval(START_TIMEOUT_MS);
    connect(&startTimeout, &QTimer::timeout, this, &StreamBroadcaster::onStartTimedOut);

    codecTimeout.setSingleShot(true);
    codecTimeout.setInterval(CODEC_TIMEOUT_MS);
    connect(&codecTimeout, &QTimer::timeout, this, &StreamBroadcaster::onCodecTimedOut);

    bitrateSettle.setSingleShot(true);
    connect(&bitrateSettle, &QTimer::timeout, this, &StreamBroadcaster::applyBitrate);

    previewUploader = new StreamPreviewUploader(
            [this](const QString &key, const QString &thumbnail, std::function<void(bool)> done) {
                this->client->uploadStreamPreview(key, thumbnail, std::move(done));
            },
            this);

    connect(streams, &StreamManager::streamChanged, this, &StreamBroadcaster::onStreamChanged);
    connect(streams, &StreamManager::streamEnded, this, &StreamBroadcaster::onStreamEnded);
    connect(voice, &Audio::VoiceManager::voiceConnected, this, &StreamBroadcaster::onVoiceConnected);
    connect(voice, &Audio::VoiceManager::voiceStateChanged, this, &StreamBroadcaster::onVoiceStateChanged);
    connect(voice, &Audio::VoiceManager::voiceDisconnected, this, &StreamBroadcaster::onVoiceDisconnected);
}

StreamBroadcaster::~StreamBroadcaster()
{
    end(false);
}

void StreamBroadcaster::setGatewaySessionId(const QString &sessionId)
{
    gatewaySessionId = sessionId;
}

bool StreamBroadcaster::canShare() const
{
    return voice->isConnected() && terms().allowed;
}

void StreamBroadcaster::reset()
{
    abandonedStreamKey.clear();
    if (isSharing())
        fail(tr("Screen sharing stopped because the connection to Discord was restarted."), false);
}

void StreamBroadcaster::setState(State state)
{
    if (currentState == state)
        return;
    currentState = state;
    emit stateChanged();
}

void StreamBroadcaster::clearFailure()
{
    if (failureText.isEmpty())
        return;
    failureText.clear();
    emit stateChanged();
}

StreamBroadcaster::Stats StreamBroadcaster::stats() const
{
    Stats now;
    now.encoder = encoderStats;
    now.network = networkStats;
    now.targetBitrate = rate.bitrate();
    now.receivers = receivers;
    now.encodingSuspended = encodingSuspended;
    now.captureStalled = captureStalled;
    if (soundCaptureFailed)
        now.audio = AudioState::Failed;
    else if (soundCapture)
        now.audio = AudioState::Capturing;
    return now;
}

void StreamBroadcaster::start(const Video::CaptureSource &chosen, const Video::SendQuality &quality, bool withAudio)
{
    if (isSharing()) {
        if (chosen.id != source.id)
            Video::releaseSource(chosen);
        return;
    }
    failureText.clear();

    const ShareTerms here = terms();
    if (!canShare()) {
        Video::releaseSource(chosen);
        fail(tr("You cannot share your screen here."));
        return;
    }
    if (!proxy.canRelayUdp()) {
        Video::releaseSource(chosen);
        fail(tr("This account's proxy cannot carry video, so the screen was not shared."));
        return;
    }

    source = chosen;
    audioWanted = withAudio;
    soundCaptureFailed = false;
    sendQuality = Video::usableQuality(quality, here.tier);
    preferredRegion = here.preferredRegion;
    streamKey = Discord::StreamKey::forChannel(voice->currentGuildId(), voice->currentChannelId(), accountId);
    abandonedStreamKey.clear();
    setState(State::Starting);

    Media::whenSendCodecsKnown(this, [this, asked = ++encoderProbeAttempt](const QList<VideoCodec> &codecs) {
        if (asked == encoderProbeAttempt)
            sendCreate(codecs);
    });
}

void StreamBroadcaster::sendCreate(const QList<VideoCodec> &codecs)
{
    if (codecs.isEmpty()) {
        fail(tr("This machine has no video encoder that Discord accepts, so it cannot share a screen."));
        return;
    }

    // The size is known already unless the window is still on its way back from the taskbar.
    const QSize size = Video::encodeSizeOf(source, sendQuality.height);
    offeredCodecs = size.isEmpty() ? codecs : Media::shareCodecs(codecs, size);
    if (offeredCodecs != codecs)
        qCInfo(LogVoice) << "Screen share" << streamKey.toString() << "does not offer AV1: its encoder would add black to" << size;
    createSent = true;
    client->sendStreamCreate(streamKey, preferredRegion);
    startTimeout.start();
}

void StreamBroadcaster::setQuality(const Video::SendQuality &quality)
{
    if (quality == sendQuality || !Video::mayUseQuality(quality, terms().tier))
        return;
    sendQuality = quality;
    if (pipeline)
        startPipeline();
}

void StreamBroadcaster::stop()
{
    end(true);
}

void StreamBroadcaster::end(bool tellGateway)
{
    if (!isSharing())
        return;

    encoderProbeAttempt++;
    startTimeout.stop();
    previewUploader->stop();

    if (rtc) {
        if (Audio::VoiceConnection *connection = rtc->connection())
            rtc->close([client = connection->client()] { client->stopVideo(); });
        // Not deleted here: this may be one of its own signals.
        std::exchange(rtc, nullptr)->deleteLater();
    }
    if (tellGateway && createSent)
        client->sendStreamDelete(streamKey.toString());
    stopPipeline();
    Video::releaseSource(source);

    createSent = false;
    sendCodec = VideoCodec::Unknown;
    receivers = -1;
    captureStalled = false;
    soundCaptureFailed = false;
    viewerSize = {};
    limits = {};
    latestPreview = QImage();
    networkStats = {};

    setState(State::Idle);
}

void StreamBroadcaster::fail(const QString &reason, bool tellGateway)
{
    qCWarning(LogVoice) << "Screen share" << streamKey.toString() << "ended:" << reason;
    failureText = reason;
    if (isSharing())
        end(tellGateway);
    else
        emit stateChanged();
    emit failed(reason);
}

void StreamBroadcaster::onStartTimedOut()
{
    // The official client gives up here without a word to the gateway.
    abandonedStreamKey = streamKey.toString();
    fail(tr("Discord did not answer the request to share your screen."), false);
}

void StreamBroadcaster::onCodecTimedOut()
{
    fail(tr("Discord named no video codec this machine can share the screen with."));
}

void StreamBroadcaster::onVoiceConnected()
{
    if (Video::screenCaptureAvailable())
        Media::whenSendCodecsKnown(this, [](const QList<VideoCodec> &) {});
}

void StreamBroadcaster::onVoiceStateChanged()
{
    if (isSharing() && voice->currentChannelId() != streamKey.channelId)
        stop();
}

void StreamBroadcaster::onVoiceDisconnected()
{
    stop();
    // The call it was said in is over.
    clearFailure();
}

void StreamBroadcaster::onStreamChanged(const QString &changedKey)
{
    if (!isSharing()) {
        if (changedKey == abandonedStreamKey) {
            abandonedStreamKey.clear();
            client->sendStreamDelete(changedKey);
        }
        return;
    }
    if (changedKey != streamKey.toString())
        return;
    const std::optional<StreamRecord> record = streams->stream(changedKey);
    if (!record)
        return;

    if (currentState == State::Starting) {
        startTimeout.stop();
        setState(State::Connecting);
        openRtc();
    }
    rtc->apply(*record);
}

void StreamBroadcaster::onStreamEnded(const QString &endedKey, StreamEndReason reason)
{
    if (!isSharing() || endedKey != streamKey.toString())
        return;

    switch (reason) {
    case StreamEndReason::Unauthorized:
    case StreamEndReason::SafetyGuildRateLimited:
        fail(tr("Discord refused the screen share."), false);
        break;
    case StreamEndReason::UserRequested:
    case StreamEndReason::StreamFull:
    case StreamEndReason::Other:
        fail(tr("Discord ended the screen share."), false);
        break;
    }
}

void StreamBroadcaster::openRtc()
{
    const StreamConnection::Account account{ accountId, gatewaySessionId, proxy };
    rtc = new StreamConnection(
            account,
            [this](Discord::Voice::ConnectionParams &params) {
                params.videoCodecs = Media::offeredVideoCodecs(offeredCodecs);
                params.sendsAudio = sendsSound();
            },
            this);
    connect(rtc, &StreamConnection::opened, this, &StreamBroadcaster::onRtcOpened);
    connect(rtc, &StreamConnection::closed, this, &StreamBroadcaster::onRtcClosed);
    connect(rtc, &StreamConnection::reconnecting, this, &StreamBroadcaster::onRtcReconnecting);
    connect(rtc, &StreamConnection::failed, this, &StreamBroadcaster::onRtcFailed);
    connect(rtc, &StreamConnection::pingRequested, this, [this] { client->sendStreamPing(streamKey.toString()); });
}

void StreamBroadcaster::onRtcOpened(Audio::VoiceConnection *connection)
{
    if (sendsSound()) {
        streamAudio = std::make_unique<StreamAudio>(connection->client(), std::max(StreamAudio::MIN_BITRATE, voice->opusBitrate()));
        connection->adopt(streamAudio.get());
    }

    connect(connection, &Audio::VoiceConnection::stateChanged, this, &StreamBroadcaster::onRtcStateChanged);
    connect(connection, &Audio::VoiceConnection::videoSendCodecChanged, this, &StreamBroadcaster::onSendCodecChanged);
    connect(connection, &Audio::VoiceConnection::videoSendStatsUpdated, this, &StreamBroadcaster::onSendStats);
    connect(connection, &Audio::VoiceConnection::videoKeyframeRequested, this, &StreamBroadcaster::onKeyframeRequested);
    connect(connection, &Audio::VoiceConnection::videoReceiversChanged, this, &StreamBroadcaster::onReceiversChanged);
    connect(connection, &Audio::VoiceConnection::videoWantsReceived, this, [this](int, int pixelCount) { onWantsReceived(pixelCount); });
    sendFramesToRtc();
}

bool StreamBroadcaster::sendsSound() const
{
    return audioWanted && Audio::processLoopbackAvailable();
}

void StreamBroadcaster::onRtcClosed()
{
    rtcConnected = false;
    videoAnnounced = false;
    serverCodec = VideoCodec::Unknown;
    codecTimeout.stop();
    QObject::disconnect(framesToRtc);
    soundCapture.reset();
    streamAudio.reset();
    applySuspension();
}

void StreamBroadcaster::onRtcReconnecting()
{
    setState(State::Reconnecting);
}

void StreamBroadcaster::onRtcFailed(StreamConnection::Failure failure)
{
    switch (failure) {
    case StreamConnection::Failure::TimedOut:
        fail(tr("The screen share could not connect to Discord's video server."));
        break;
    case StreamConnection::Failure::Lost:
        fail(tr("The screen share lost its connection to Discord's video server."));
        break;
    case StreamConnection::Failure::ProxyRefused:
        fail(tr("This account's proxy cannot carry video, so the screen share was stopped."));
        break;
    }
}

void StreamBroadcaster::startSoundCapture()
{
    if (!streamAudio || soundCapture || soundCaptureFailed)
        return;

    const auto self = static_cast<quint32>(QCoreApplication::applicationPid());
    const bool ofWindow = source.kind == Video::CaptureSource::Kind::Window;
    const bool ofProgram = ofWindow && !(source.processId == 0 && Video::isHandedOverByTheDesktop(source));
    if (ofProgram && source.processId == 0) {
        onSoundCaptureFailed(tr("The program the shared window belongs to is not known."));
        return;
    }
    // All this client plays is the call, and the people in it would get themselves back.
    if (ofProgram && source.processId == self)
        return;

    soundCapture = std::make_unique<Audio::LoopbackCapture>();
    connect(soundCapture.get(), &Audio::LoopbackCapture::frameCaptured, streamAudio.get(), &StreamAudio::onFrameCaptured);
    // Context the capture, so that a failure it had on its way when it was let go of goes with it.
    connect(soundCapture.get(), &Audio::LoopbackCapture::failed, soundCapture.get(), [this](const QString &reason) { onSoundCaptureFailed(reason); });
    if (ofProgram)
        soundCapture->start(source.processId, Audio::LoopbackCapture::Scope::ProcessTree);
    else
        soundCapture->start(self, Audio::LoopbackCapture::Scope::AllButProcessTree);
    emit stateChanged();
}

void StreamBroadcaster::onSoundCaptureFailed(const QString &reason)
{
    qCWarning(LogVoice) << "Screen share" << streamKey.toString() << "goes on without sound:" << reason;
    soundCaptureFailed = true;
    soundCapture.reset();
    emit stateChanged();
}

// Reached on the first connection and again whenever the client has resumed a dropped one,
// which has forgotten what it was told of the video.
void StreamBroadcaster::onRtcStateChanged(VoiceClient::State state)
{
    if (state != VoiceClient::State::Connected)
        return;

    rtcConnected = true;
    videoAnnounced = false;
    startSoundCapture();
    useServerCodec();
}

// The server names the codec as the session is described, which is before the connection is up.
void StreamBroadcaster::onSendCodecChanged(VideoCodec codec)
{
    serverCodec = codec;
    if (rtcConnected)
        useServerCodec();
}

void StreamBroadcaster::useServerCodec()
{
    if (serverCodec == VideoCodec::Unknown)
        return;

    codecTimeout.stop();
    if (pipeline && serverCodec == sendCodec) {
        if (pipelineStarted && !videoAnnounced)
            announceVideo();
        return;
    }
    sendCodec = serverCodec;
    startPipeline();
}

// The pipeline encodes nothing of a size its encoder would add black to, so nothing of it is
// sent while the server takes its time to name another codec.
void StreamBroadcaster::withdrawCodecsUnfitFor(const QSize &size)
{
    const QList<VideoCodec> fit = Media::shareCodecs(offeredCodecs, size);
    if (fit == offeredCodecs)
        return;

    qCInfo(LogVoice) << "Screen share" << streamKey.toString() << "withdraws AV1: its encoder would add black to" << size;
    offeredCodecs = fit;
    if (!rtc || !rtc->connection())
        return;
    rtc->connection()->run([fit](VoiceClient *connection) { connection->offerVideoEncoding(fit); });
    if (!fit.contains(sendCodec))
        codecTimeout.start();
}

void StreamBroadcaster::onSendStats(const VideoSendStats &sent)
{
    networkStats = sent;
    if (!pipelineStarted)
        return;
    const bool limited = rate.setReceiverLimit(Video::estimatedCeiling(sent.receiverEstimateBps, sent.delayEstimateBps));
    if (rate.report(sent.lossFraction, rateClock.elapsed()) || limited)
        applyBitrateOnceSettled();
}

void StreamBroadcaster::onKeyframeRequested()
{
    if (pipeline)
        pipeline->requestKeyframe();
}

void StreamBroadcaster::onReceiversChanged(int count)
{
    receivers = count;
    applySuspension();
}

void StreamBroadcaster::onWantsReceived(int pixelCount)
{
    if (viewerSize.offer(pixelCount))
        refreshLimits();
}

void StreamBroadcaster::startPipeline()
{
    stopPipeline();
    // A capture that is starting has not stalled, whatever the one before it had done.
    onCaptureStalled(false);

    limits = Video::bitrateLimits(sendQuality);
    rate.restart(limits.min, limits.max);

    pipeline = std::make_unique<Video::BroadcastPipeline>();
    // In the pipeline's own context, so that what it had on its way when it was let go of
    // goes with it.
    Video::BroadcastPipeline *started = pipeline.get();
    connect(started, &Video::BroadcastPipeline::started, started, [this](const QSize &size, int framerate) { onPipelineStarted(size, framerate); });
    connect(started, &Video::BroadcastPipeline::previewReady, started, [this](const QImage &preview) { onPreviewReady(preview); });
    connect(started, &Video::BroadcastPipeline::captureStalled, started, [this](bool stalled) { onCaptureStalled(stalled); });
    connect(started, &Video::BroadcastPipeline::sourceClosed, started, [this] { onSourceClosed(); });
    connect(started, &Video::BroadcastPipeline::failed, started, [this](const QString &reason) { onPipelineFailed(reason); });
    connect(started, &Video::BroadcastPipeline::statsUpdated, started, [this](const Video::BroadcastStats &encoder) { onPipelineStats(encoder); });
    sendFramesToRtc();

    Video::BroadcastSettings settings;
    settings.sourceId = source.id;
    settings.maxHeight = sendQuality.height;
    settings.framerate = sendQuality.framerate;
    settings.codec = sendCodec;
    settings.bitrate = rate.bitrate();
    settings.maxBitrate = limits.max;
    pipeline->start(settings);
    appliedBitrate = settings.bitrate;
    sinceBitrateApplied.start();
    tellBitrate();
}

void StreamBroadcaster::stopPipeline()
{
    if (!pipeline)
        return;

    bitrateSettle.stop();
    QObject::disconnect(framesToRtc);
    pipeline->stop();
    pipeline.reset();

    pipelineStarted = false;
    encodingSuspended = false;
    encodedSize = QSize();
    encodedFramerate = 0;
    encoderStats = {};
}

void StreamBroadcaster::sendFramesToRtc()
{
    QObject::disconnect(framesToRtc);
    if (!pipeline || !rtc || !rtc->connection())
        return;

    VoiceClient *connection = rtc->connection()->client();
    framesToRtc = connect(pipeline.get(), &Video::BroadcastPipeline::frameEncoded, connection, [connection](const Video::EncodedFramePointer &frame) {
        connection->sendVideoFrame(*frame);
    });
}

void StreamBroadcaster::onPipelineStarted(const QSize &size, int framerate)
{
    pipelineStarted = true;
    encodedSize = size;
    encodedFramerate = framerate;
    withdrawCodecsUnfitFor(size);
    refreshLimits();

    if (rtcConnected && !videoAnnounced)
        announceVideo();
    applySuspension();
}

void StreamBroadcaster::onPreviewReady(const QImage &preview)
{
    latestPreview = preview;
    previewUploader->offer(preview);
    emit previewChanged(preview);
}

void StreamBroadcaster::onCaptureStalled(bool stalled)
{
    if (stalled == captureStalled)
        return;

    captureStalled = stalled;
    previewUploader->setStalled(stalled);
    client->sendStreamSetPaused(streamKey.toString(), stalled);
    emit stateChanged();
}

// Closing the shared window ends the share as stopping it does, in the official client too.
void StreamBroadcaster::onSourceClosed()
{
    stop();
}

void StreamBroadcaster::onPipelineFailed(const QString &reason)
{
    fail(tr("Screen sharing stopped: %1").arg(reason));
}

void StreamBroadcaster::onPipelineStats(const Video::BroadcastStats &encoder)
{
    encoderStats = encoder;
}

LocalVideoDescription StreamBroadcaster::videoDescription() const
{
    const QSize largest = Video::announcedSize(sendQuality);
    LocalVideoDescription description;
    description.maxWidth = largest.width();
    description.maxHeight = largest.height();
    description.maxFramerate = sendQuality.framerate;
    // The quality's own limit, whatever the viewers' sizes have lowered the encoder to: the
    // official client announces again only when the quality itself is changed.
    description.maxBitrate = Video::bitrateLimits(sendQuality).max;
    return description;
}

void StreamBroadcaster::announceVideo()
{
    announced = videoDescription();
    videoAnnounced = true;
    rtc->connection()->run([description = announced](VoiceClient *connection) { connection->startVideo(description); });
    tellBitrate();
    applySuspension();
    // What was encoded before the client took the announcement was dropped by it, and that
    // included the frame every viewer needs first.
    pipeline->requestKeyframe();

    previewUploader->start(streamKey.toString());
    setState(State::Live);
}

void StreamBroadcaster::describeVideoIfChanged()
{
    if (!videoAnnounced || !rtcConnected)
        return;
    const LocalVideoDescription description = videoDescription();
    if (sameDescription(description, announced))
        return;

    announced = description;
    rtc->connection()->run([description](VoiceClient *connection) { connection->describeVideo(description); });
}

void StreamBroadcaster::refreshLimits()
{
    const Video::BitrateLimits full = Video::bitrateLimits(sendQuality);
    const qint64 encodedPixels = static_cast<qint64>(encodedSize.width()) * encodedSize.height();
    limits = pipelineStarted ? Video::limitsForViewerSize(full, sendQuality, encodedPixels, encodedFramerate, viewerSize.pixelCount()) : full;

    if (rate.setLimits(limits.min, limits.max) && pipelineStarted)
        applyBitrateOnceSettled();
    describeVideoIfChanged();
}

void StreamBroadcaster::applyBitrateOnceSettled()
{
    if (bitrateSettle.isActive())
        return;

    const qint64 sinceLast = sinceBitrateApplied.elapsed();
    if (sinceLast >= BITRATE_SETTLE_MS)
        applyBitrate();
    else
        bitrateSettle.start(static_cast<int>(BITRATE_SETTLE_MS - sinceLast));
}

void StreamBroadcaster::applyBitrate()
{
    if (!pipeline || !pipelineStarted || rate.bitrate() == appliedBitrate)
        return;

    appliedBitrate = rate.bitrate();
    sinceBitrateApplied.start();
    pipeline->setBitrate(appliedBitrate);
    tellBitrate();
}

void StreamBroadcaster::tellBitrate()
{
    if (!rtc || !rtc->connection() || appliedBitrate <= 0)
        return;
    rtc->connection()->run([bitrate = appliedBitrate](VoiceClient *connection) { connection->setVideoBitrate(bitrate); });
}

// Nothing is encoded while nobody is there to receive it, as in the official client. Only once
// the pipeline has started, though: it is not promised to report a start while it encodes
// nothing.
void StreamBroadcaster::applySuspension()
{
    const bool suspend = pipelineStarted && (!videoAnnounced || receivers == 0);
    if (suspend == encodingSuspended)
        return;
    encodingSuspended = suspend;
    pipeline->setEncodingSuspended(suspend);
}

} // namespace Core
} // namespace Acheron
