#include "StreamSession.hpp"

#include "Core/Audio/VoiceConnection.hpp"
#include "Core/Audio/VoiceManager.hpp"
#include "Core/Logging.hpp"
#include "Core/Media/CodecSupport.hpp"

#include <algorithm>

namespace Acheron {
namespace Core {

using Discord::Voice::VoiceClient;

StreamSession::StreamSession(const Discord::StreamKey &key, Snowflake accountId, const QString &gatewaySessionId, const ProxyConfig &proxy, Audio::VoiceManager *voice, quint32 audioSource, QObject *parent)
    : QObject(parent), streamKey(key), accountId(accountId), gatewaySessionId(gatewaySessionId), proxy(proxy), voice(voice), audioSource(audioSource)
{
    connectTimeout.setSingleShot(true);
    connect(&connectTimeout, &QTimer::timeout, this, [this] {
        if (currentState != State::Connecting)
            return;
        qCWarning(LogVoice) << "Stream" << streamKey.toString() << "did not connect in time";
        disconnectRtc();
        setState(State::Failed);
    });
    connectTimeout.start(CONNECT_TIMEOUT_MS);

    surfaceDebounce.setSingleShot(true);
    surfaceDebounce.setInterval(SURFACE_DEBOUNCE_MS);
    connect(&surfaceDebounce, &QTimer::timeout, this, &StreamSession::pushWants);

    reconnectDelay.setSingleShot(true);
    reconnectDelay.setInterval(RECONNECT_DELAY_MS);
    connect(&reconnectDelay, &QTimer::timeout, this, [this] {
        if (currentState == State::Reconnecting && !endpoint.isEmpty())
            connectRtc();
    });
}

StreamSession::~StreamSession()
{
    disconnectRtc();
}

void StreamSession::setState(State state)
{
    if (currentState == state)
        return;
    currentState = state;
    if (state != State::Connecting)
        connectTimeout.stop();
    emit stateChanged();
}

void StreamSession::apply(const StreamRecord &record)
{
    if (isOver())
        return;

    if (viewers != record.viewerIds) {
        viewers = record.viewerIds;
        emit viewersChanged();
    }

    paused = record.paused;
    if (rtcConnected && (currentState == State::Active || currentState == State::Paused))
        setState(paused ? State::Paused : State::Active);

    switch (record.state) {
    case StreamRecord::State::AwaitingEndpoint:
        // The server is moving the stream; the next update says where to.
        if (connection && record.endpoint.isEmpty() && !endpoint.isEmpty()) {
            disconnectRtc();
            endpoint.clear();
            setState(State::Reconnecting);
        }
        break;
    case StreamRecord::State::Reconnecting:
        disconnectRtc();
        endpoint.clear();
        setState(State::Reconnecting);
        break;
    case StreamRecord::State::Ready:
        if (connection && record.endpoint == endpoint && record.token == token)
            break;
        rtcServerId = record.rtcServerId;
        rtcChannelId = record.rtcChannelId;
        endpoint = record.endpoint;
        token = record.token;
        reconnects = 0;
        connectRtc();
        break;
    }
}

void StreamSession::end(StreamEndReason reason)
{
    disconnectRtc();
    switch (reason) {
    case StreamEndReason::StreamFull:
        setState(State::Full);
        break;
    case StreamEndReason::Unauthorized:
    case StreamEndReason::SafetyGuildRateLimited:
        setState(State::Failed);
        break;
    case StreamEndReason::UserRequested:
    case StreamEndReason::Other:
        setState(State::Ended);
        break;
    }
}

void StreamSession::connectRtc()
{
    disconnectRtc();

    Discord::Voice::ConnectionParams params;
    params.endpoint = endpoint;
    params.token = token;
    params.serverId = rtcServerId;
    // A stream's RTC channel is the id one below its RTC server when the gateway names none.
    params.channelId = rtcChannelId.isValid() ? rtcChannelId : Snowflake(static_cast<quint64>(rtcServerId) - 1);
    params.userId = accountId;
    params.sessionId = gatewaySessionId;
    params.proxy = proxy;
    params.video = true;
    Discord::Voice::StreamDescription screen;
    screen.type = QStringLiteral("screen");
    screen.rid = QStringLiteral("100");
    screen.quality = 100;
    params.streams = { screen };
    params.videoCodecs = Media::offeredVideoCodecs();
    params.sendsAudio = false;
    params.streamContext = true;

    connection = new Audio::VoiceConnection(params, this);
    VoiceClient *client = connection->client();

    decoder = std::make_unique<Media::VideoDecoder>();
    decoder->setSink(frameSink);
    // The decoder outlives every call the client can still make: disconnectRtc() stops the
    // client before it frees the decoder.
    decoder->setKeyframeRequester([client] { QMetaObject::invokeMethod(client, &VoiceClient::requestVideoKeyframeFromEveryone); });
    decoder->setCodecFailureHandler([client](Discord::Voice::VideoCodec codec) { QMetaObject::invokeMethod(client, [client, codec] { client->withdrawVideoCodec(codec); }); });
    client->setVideoFrameHandler([target = decoder.get()](Snowflake, Discord::Voice::VideoCodec codec, Discord::Voice::EncodedVideoFrame frame) {
        target->push(codec, std::move(frame));
    });

    connect(client, &VoiceClient::connected, this, &StreamSession::onRtcConnected);
    connect(client, &VoiceClient::disconnected, this, &StreamSession::onRtcDisconnected, Qt::QueuedConnection);
    connect(client, &VoiceClient::remoteVideoChanged, this, &StreamSession::onRemoteVideoChanged);
    connect(client, &VoiceClient::videoStatsUpdated, this, &StreamSession::onVideoStats);

    voice->attachAudioSource(audioSource, client);
    voice->setAudioSourceVolume(audioSource, audioVolume);

    statsClock.invalidate();
    statsBytes = 0;
    statsDecoded = 0;
    demand = {};
    demandClock.start();
    fullPicturePixels = 0;

    if (!connection->start()) {
        disconnectRtc();
        setState(State::Failed);
    }
}

void StreamSession::disconnectRtc()
{
    if (!connection)
        return;

    rtcConnected = false;
    reconnectDelay.stop();
    voice->detachAudioSource(audioSource);

    connection->client()->disconnect(this);
    connection->stop();
    decoder.reset();
    delete connection;
    connection = nullptr;
}

void StreamSession::onRtcConnected()
{
    if (sender() != (connection ? connection->client() : nullptr))
        return;

    rtcConnected = true;
    reconnects = 0;
    setState(paused ? State::Paused : State::Active);
    pushWants();
}

void StreamSession::onRtcDisconnected()
{
    if (!connection || sender() != connection->client() || isOver())
        return;

    qCWarning(LogVoice) << "Stream" << streamKey.toString() << "lost its RTC connection";
    disconnectRtc();

    if (++reconnects > MAX_RECONNECTS) {
        setState(State::Failed);
        return;
    }
    setState(State::Reconnecting);
    emit pingRequested();
    reconnectDelay.start();
}

void StreamSession::onRemoteVideoChanged(Snowflake userId, bool active, int maxWidth, int maxHeight, int maxFramerate)
{
    if (userId != streamKey.ownerId || !active)
        return;

    const QSize size(maxWidth, maxHeight);
    if (size == announcedSize && maxFramerate == announcedFramerate)
        return;
    announcedSize = size;
    announcedFramerate = maxFramerate;
    emit sourceChanged();
}

void StreamSession::onVideoStats(const Discord::Voice::VideoReceiveStats &stats)
{
    if (!decoder)
        return;

    latestStats.network = stats;
    latestStats.decoder = decoder->stats();

    if (statsClock.isValid()) {
        const double seconds = static_cast<double>(statsClock.restart()) / 1000.0;
        if (seconds > 0) {
            latestStats.bitrateKbps = static_cast<double>(stats.bytes - statsBytes) * 8.0 / 1000.0 / seconds;
            latestStats.framesPerSecond = static_cast<double>(latestStats.decoder.decoded - statsDecoded) / seconds;
        }
    } else {
        statsClock.start();
    }
    statsBytes = stats.bytes;
    statsDecoded = latestStats.decoder.decoded;

    fullPicturePixels = std::max({ fullPicturePixels, static_cast<qint64>(latestStats.decoder.width) * latestStats.decoder.height, static_cast<qint64>(announcedSize.width()) * announcedSize.height() });
    if (demand.observe(latestStats.decoder.backlogMs, latestStats.decoder.queuesDiscarded, demandClock.elapsed())) {
        qCInfo(LogVoice) << "Stream" << streamKey.toString() << "decoder backlog" << latestStats.decoder.backlogMs << "ms; now asking for" << demand.pixelShare() << "of the picture";
        pushWants();
    }
    latestStats.pixelShareAskedFor = demand.pixelShare();

    emit statsChanged();
}

void StreamSession::setSink(std::shared_ptr<Media::VideoFrameSink> sink)
{
    frameSink = std::move(sink);
    if (decoder)
        decoder->setSink(frameSink);
}

void StreamSession::setSurface(int pixelCount, bool visible)
{
    if (pixelCount == surfacePixels && visible == surfaceVisible)
        return;

    // Resizes are batched, but coming into view should not wait.
    const bool appeared = visible && !surfaceVisible;
    surfacePixels = pixelCount;
    surfaceVisible = visible;
    if (appeared) {
        surfaceDebounce.stop();
        pushWants();
    } else {
        surfaceDebounce.start();
    }
}

void StreamSession::pushWants()
{
    if (!connection || !rtcConnected)
        return;

    const int pixels = demand.pixelsToAskFor(surfacePixels, fullPicturePixels);
    const bool wanted = surfaceVisible;
    VoiceClient *client = connection->client();
    QMetaObject::invokeMethod(client, [client, pixels, wanted] { client->setVideoWants(pixels, wanted); });
}

void StreamSession::setVolume(float volume)
{
    audioVolume = volume;
    if (connection)
        voice->setAudioSourceVolume(audioSource, volume);
}

} // namespace Core
} // namespace Acheron
