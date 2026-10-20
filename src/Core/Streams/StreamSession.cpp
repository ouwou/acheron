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
    : QObject(parent), streamKey(key), voice(voice), audioSource(audioSource)
{
    const StreamConnection::Account account{ accountId, gatewaySessionId, proxy };
    rtc = new StreamConnection(account, [](Discord::Voice::ConnectionParams &params) { params.videoCodecs = Media::offeredVideoCodecs(); }, this);
    connect(rtc, &StreamConnection::opened, this, &StreamSession::onRtcOpened);
    connect(rtc, &StreamConnection::connected, this, &StreamSession::onRtcConnected);
    connect(rtc, &StreamConnection::closed, this, &StreamSession::onRtcClosed);
    connect(rtc, &StreamConnection::reconnecting, this, &StreamSession::onRtcReconnecting);
    connect(rtc, &StreamConnection::failed, this, &StreamSession::onRtcFailed);
    connect(rtc, &StreamConnection::pingRequested, this, &StreamSession::pingRequested);

    surfaceDebounce.setSingleShot(true);
    surfaceDebounce.setInterval(SURFACE_DEBOUNCE_MS);
    connect(&surfaceDebounce, &QTimer::timeout, this, &StreamSession::pushWants);
}

StreamSession::~StreamSession()
{
    rtc->close();
}

void StreamSession::setState(State state)
{
    if (currentState == state)
        return;
    currentState = state;
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
    if (rtc->isConnected() && (currentState == State::Active || currentState == State::Paused))
        setState(paused ? State::Paused : State::Active);

    rtc->apply(record);
}

void StreamSession::end(StreamEndReason reason)
{
    rtc->close();
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

void StreamSession::onRtcOpened(Audio::VoiceConnection *connection)
{
    VoiceClient *client = connection->client();

    decoder = std::make_unique<Media::VideoDecoder>();
    decoder->setSink(frameSink);
    // The decoder outlives every call the client can still make: the connection is stopped
    // before the decoder is let go of.
    decoder->setKeyframeRequester([client] { QMetaObject::invokeMethod(client, &VoiceClient::requestVideoKeyframeFromEveryone); });
    decoder->setCodecFailureHandler([client](Discord::Voice::VideoCodec codec) { QMetaObject::invokeMethod(client, [client, codec] { client->withdrawVideoCodec(codec); }); });
    client->setVideoFrameHandler([target = decoder.get()](Snowflake, Discord::Voice::VideoCodec codec, Discord::Voice::EncodedVideoFrame frame) {
        target->push(codec, std::move(frame));
    });

    connect(connection, &Audio::VoiceConnection::remoteVideoChanged, this, &StreamSession::onRemoteVideoChanged);
    connect(connection, &Audio::VoiceConnection::videoStatsUpdated, this, &StreamSession::onVideoStats);

    voice->attachAudioSource(audioSource, client);
    voice->setAudioSourceVolume(audioSource, audioVolume);

    statsClock.invalidate();
    statsBytes = 0;
    statsDecoded = 0;
    demand = {};
    demandClock.start();
    fullPicturePixels = 0;
}

void StreamSession::onRtcConnected()
{
    setState(paused ? State::Paused : State::Active);
    pushWants();
}

void StreamSession::onRtcClosed()
{
    voice->detachAudioSource(audioSource);
    decoder.reset();
}

void StreamSession::onRtcReconnecting()
{
    if (!isOver())
        setState(State::Reconnecting);
}

void StreamSession::onRtcFailed()
{
    if (!isOver())
        setState(State::Failed);
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
    if (!rtc->isConnected())
        return;

    const int pixels = demand.pixelsToAskFor(surfacePixels, fullPicturePixels);
    const bool wanted = surfaceVisible;
    rtc->connection()->run([pixels, wanted](VoiceClient *client) { client->setVideoWants(pixels, wanted); });
}

void StreamSession::setVolume(float volume)
{
    audioVolume = volume;
    if (rtc->connection())
        voice->setAudioSourceVolume(audioSource, volume);
}

} // namespace Core
} // namespace Acheron
