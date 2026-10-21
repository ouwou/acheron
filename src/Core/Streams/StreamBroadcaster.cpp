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
    return a.maxSize == b.maxSize && a.maxFramerate == b.maxFramerate && a.maxBitrateBps == b.maxBitrateBps;
}

} // namespace

StreamBroadcaster::StreamBroadcaster(Discord::Client *client, StreamManager *streams, Audio::VoiceManager *voice, TermsProvider terms, QObject *parent)
    : QObject(parent), client(client), streams(streams), voice(voice), terms(std::move(terms))
{
    rateClock.start();

    startTimeout.setSingleShot(true);
    startTimeout.setInterval(START_TIMEOUT_MS);
    connect(&startTimeout, &QTimer::timeout, this, &StreamBroadcaster::onStartTimedOut);

    codecTimeout.setSingleShot(true);
    codecTimeout.setInterval(Video::SEND_CODEC_TIMEOUT_MS);
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
    end(GatewayNotice::Skip);
}

bool StreamBroadcaster::canShare() const
{
    return voice->isConnected() && terms().allowed;
}

void StreamBroadcaster::reset()
{
    abandonedStreamKey.clear();
    if (isSharing())
        fail(tr("Screen sharing stopped because the connection to Discord was restarted."), GatewayNotice::Skip);
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
    now.targetBitrateBps = rate.bitrate();
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
    if (!voice->accountProxy().canRelayUdp()) {
        Video::releaseSource(chosen);
        fail(tr("This account's proxy cannot carry video, so the screen was not shared."));
        return;
    }

    source = chosen;
    audioWanted = withAudio;
    soundCaptureFailed = false;
    sendQuality = Video::usableQuality(quality, here.tier);
    preferredRegion = here.preferredRegion;
    streamKey = voice->streamKeyOf(voice->accountUserId());
    abandonedStreamKey.clear();
    setState(State::Starting);

    sendCodecs.ask(this, [this](const QList<VideoCodec> &codecs) { sendCreate(codecs); });
}

void StreamBroadcaster::sendCreate(const QList<VideoCodec> &codecs)
{
    if (codecs.isEmpty()) {
        fail(tr("This machine has no video encoder that Discord accepts, so it cannot share a screen."));
        return;
    }

    const QSize size = Video::encodeSizeOf(source, sendQuality.heightLimit());
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
    end(GatewayNotice::Send);
}

void StreamBroadcaster::end(GatewayNotice notice)
{
    if (!isSharing())
        return;

    sendCodecs.cancel();
    startTimeout.stop();
    previewUploader->stop();

    if (rtc) {
        if (Audio::VoiceConnection *connection = rtc->connection())
            rtc->close([client = connection->client()] { client->stopVideo(); });
        // not deleted here: this may be one of its own signals
        std::exchange(rtc, nullptr)->deleteLater();
    }
    if (notice == GatewayNotice::Send && createSent)
        client->sendStreamDelete(streamKey.toString());
    stopPipeline();
    Video::releaseSource(source);

    createSent = false;
    sendCodec = VideoCodec::Unknown;
    receivers.reset();
    captureStalled = false;
    soundCaptureFailed = false;
    viewerSize = {};
    limits = {};
    latestPreview = QImage();
    networkStats = {};

    setState(State::Idle);
}

void StreamBroadcaster::fail(const QString &reason, GatewayNotice notice)
{
    qCWarning(LogVoice) << "Screen share" << streamKey.toString() << "ended:" << reason;
    failureText = reason;
    if (isSharing())
        end(notice);
    else
        emit stateChanged();
    emit failed(reason);
}

void StreamBroadcaster::onStartTimedOut()
{
    // the official client gives up here without telling the gateway
    abandonedStreamKey = streamKey.toString();
    fail(tr("Discord did not answer the request to share your screen."), GatewayNotice::Skip);
}

void StreamBroadcaster::onCodecTimedOut()
{
    fail(tr("Discord named no video codec this machine can share the screen with."));
}

void StreamBroadcaster::onVoiceConnected()
{
    if (Video::screenCaptureAvailable())
        Media::startSendCodecProbe();
}

void StreamBroadcaster::onVoiceStateChanged()
{
    if (isSharing() && voice->currentChannelId() != streamKey.channelId)
        stop();
}

void StreamBroadcaster::onVoiceDisconnected()
{
    stop();
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

    fail(isRefusal(reason) ? tr("Discord refused the screen share.") : tr("Discord ended the screen share."), GatewayNotice::Skip);
}

void StreamBroadcaster::openRtc()
{
    rtc = new StreamConnection(
            StreamConnection::Account::of(*voice),
            [this](Discord::Voice::ConnectionParams &params) {
                params.videoCodecs = Media::offeredVideoCodecs(offeredCodecs);
                params.sendsAudio = sendsSound();
            },
            this);
    connect(rtc, &StreamConnection::opened, this, &StreamBroadcaster::onRtcOpened);
    connect(rtc, &StreamConnection::connected, this, &StreamBroadcaster::onRtcConnected);
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
    // this client plays only the call, which would echo the participants back to themselves
    if (ofProgram && source.processId == self)
        return;

    soundCapture = std::make_unique<Audio::LoopbackCapture>();
    connect(soundCapture.get(), &Audio::LoopbackCapture::frameCaptured, streamAudio.get(), &StreamAudio::onFrameCaptured);
    // the capture is the context, so a failure still on its way goes with it
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

void StreamBroadcaster::onRtcConnected()
{
    videoAnnounced = false;
    startSoundCapture();
    useServerCodec();
}

void StreamBroadcaster::onSendCodecChanged(VideoCodec codec)
{
    serverCodec = codec;
    if (rtc->isConnected())
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

void StreamBroadcaster::withdrawCodecsUnfitFor(const QSize &size)
{
    const QList<VideoCodec> fit = Media::shareCodecs(offeredCodecs, size);
    if (fit == offeredCodecs)
        return;

    qCInfo(LogVoice) << "Screen share" << streamKey.toString() << "withdraws AV1: its encoder would add black to" << size;
    offeredCodecs = fit;
    if (!rtc || !rtc->connection())
        return;
    rtc->run([fit](VoiceClient *connection) { connection->offerVideoEncoding(fit); });
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
    onCaptureStalled(false);

    limits = Video::bitrateLimits(sendQuality);
    rate.restart(limits.minBps, limits.maxBps);

    pipeline = std::make_unique<Video::ScreenPipeline>();
    // the pipeline is the context, so whatever it still had on the way goes with it
    Video::ScreenPipeline *started = pipeline.get();
    connect(started, &Video::ScreenPipeline::started, started, [this](const QSize &size, int framerate) { onPipelineStarted(size, framerate); });
    connect(started, &Video::ScreenPipeline::previewReady, started, [this](const QImage &preview) { onPreviewReady(preview); });
    connect(started, &Video::ScreenPipeline::captureStalled, started, [this](bool stalled) { onCaptureStalled(stalled); });
    connect(started, &Video::ScreenPipeline::sourceClosed, started, [this] { onSourceClosed(); });
    connect(started, &Video::ScreenPipeline::failed, started, [this](const QString &reason) { onPipelineFailed(reason); });
    connect(started, &Video::ScreenPipeline::statsUpdated, started, [this](const Video::PipelineStats &encoder) { onPipelineStats(encoder); });
    sendFramesToRtc();

    Video::ScreenSettings settings;
    settings.sourceId = source.id;
    settings.maxHeight = sendQuality.heightLimit();
    settings.framerate = sendQuality.framerate;
    settings.codec = sendCodec;
    appliedBitrate = rate.bitrate();
    settings.bitrateBps = appliedBitrate;
    settings.maxBitrateBps = limits.maxBps;
    pipeline->start(settings);
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
    framesToRtc = connect(pipeline.get(), &Video::ScreenPipeline::frameEncoded, connection, [connection](const Video::EncodedFramePointer &frame) {
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

    if (rtc && rtc->isConnected() && !videoAnnounced)
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

// as in the official client
void StreamBroadcaster::onSourceClosed()
{
    stop();
}

void StreamBroadcaster::onPipelineFailed(const QString &reason)
{
    fail(tr("Screen sharing stopped: %1").arg(reason));
}

void StreamBroadcaster::onPipelineStats(const Video::PipelineStats &encoder)
{
    encoderStats = encoder;
}

LocalVideoDescription StreamBroadcaster::videoDescription() const
{
    LocalVideoDescription description;
    description.maxSize = Video::announcedSize(sendQuality);
    description.maxFramerate = sendQuality.framerate;
    // the official client re-announces only when the quality changes
    description.maxBitrateBps = Video::bitrateLimits(sendQuality).maxBps;
    return description;
}

void StreamBroadcaster::announceVideo()
{
    announced = videoDescription();
    videoAnnounced = true;
    rtc->run([description = announced](VoiceClient *connection) { connection->startVideo(description); });
    tellBitrate();
    applySuspension();
    // the client dropped what came before it took the announcement, including the first key frame
    pipeline->requestKeyframe();

    previewUploader->start(streamKey.toString());
    setState(State::Live);
}

void StreamBroadcaster::describeVideoIfChanged()
{
    if (!videoAnnounced || !rtc || !rtc->isConnected())
        return;
    const LocalVideoDescription description = videoDescription();
    if (sameDescription(description, announced))
        return;

    announced = description;
    rtc->run([description](VoiceClient *connection) { connection->describeVideo(description); });
}

void StreamBroadcaster::refreshLimits()
{
    const Video::BitrateLimits full = Video::bitrateLimits(sendQuality);
    const qint64 encodedPixels = static_cast<qint64>(encodedSize.width()) * encodedSize.height();
    limits = pipelineStarted ? Video::limitsForViewerSize(full, sendQuality, encodedPixels, encodedFramerate, viewerSize.pixelCount()) : full;

    if (rate.setLimits(limits.minBps, limits.maxBps) && pipelineStarted)
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
    if (rtc && appliedBitrate > 0)
        rtc->run([bitrate = appliedBitrate](VoiceClient *connection) { connection->setVideoBitrate(bitrate); });
}

void StreamBroadcaster::applySuspension()
{
    const bool suspend = Video::encodingSuspends(pipelineStarted, videoAnnounced, receivers);
    if (suspend == encodingSuspended)
        return;
    encodingSuspended = suspend;
    pipeline->setEncodingSuspended(suspend);
}

} // namespace Core
} // namespace Acheron
