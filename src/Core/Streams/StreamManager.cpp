#include "StreamManager.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Core {

StreamEndReason streamEndReasonFromString(const QString &reason)
{
    if (reason == QLatin1String("user_requested"))
        return StreamEndReason::UserRequested;
    if (reason == QLatin1String("stream_full"))
        return StreamEndReason::StreamFull;
    if (reason == QLatin1String("unauthorized"))
        return StreamEndReason::Unauthorized;
    if (reason == QLatin1String("safety_guild_rate_limited"))
        return StreamEndReason::SafetyGuildRateLimited;
    return StreamEndReason::Other;
}

StreamManager::StreamManager(QObject *parent) : QObject(parent) { }

void StreamManager::handleStreamCreate(const Discord::StreamCreate &event)
{
    const auto key = Discord::StreamKey::parse(event.streamKey);
    if (!key) {
        qCWarning(LogDiscord) << "STREAM_CREATE with a stream key this client cannot read:" << event.streamKey.get();
        return;
    }

    StreamRecord record;
    record.key = *key;
    record.region = event.region.valueOr();
    record.viewerIds = event.viewerIds;
    record.paused = event.paused;
    if (event.rtcServerId.hasValue())
        record.rtcServerId = event.rtcServerId.get();
    if (event.rtcChannelId.hasValue())
        record.rtcChannelId = event.rtcChannelId.get();

    streams.insert(event.streamKey.get(), record);
    emit streamChanged(event.streamKey.get());
}

void StreamManager::handleStreamServerUpdate(const Discord::StreamServerUpdate &event)
{
    auto it = streams.find(event.streamKey.get());
    if (it == streams.end())
        return;

    if (event.endpoint.hasValue() && !event.endpoint->isEmpty()) {
        it->endpoint = event.endpoint.get();
        it->token = event.token;
        it->state = StreamRecord::State::Ready;
    } else {
        it->endpoint.clear();
        it->state = StreamRecord::State::AwaitingEndpoint;
    }

    emit streamChanged(event.streamKey);
}

void StreamManager::handleStreamUpdate(const Discord::StreamUpdate &event)
{
    auto it = streams.find(event.streamKey.get());
    if (it == streams.end())
        return;

    it->region = event.region.valueOr();
    it->viewerIds = event.viewerIds;
    it->paused = event.paused;

    emit streamChanged(event.streamKey);
}

void StreamManager::handleStreamDelete(const Discord::StreamDelete &event)
{
    auto it = streams.find(event.streamKey.get());
    if (it == streams.end())
        return;

    if (event.unavailable.valueOr(false)) {
        it->state = StreamRecord::State::Reconnecting;
        it->endpoint.clear();
        emit streamChanged(event.streamKey);
        return;
    }

    streams.erase(it);
    emit streamEnded(event.streamKey, streamEndReasonFromString(event.reason.valueOr()));
}

void StreamManager::reset()
{
    const QStringList keys = streams.keys();
    streams.clear();
    for (const QString &key : keys)
        emit streamEnded(key, StreamEndReason::Other);
}

std::optional<StreamRecord> StreamManager::stream(const QString &streamKey) const
{
    auto it = streams.constFind(streamKey);
    if (it == streams.constEnd())
        return std::nullopt;
    return it.value();
}

QStringList StreamManager::streamKeys() const
{
    return streams.keys();
}

} // namespace Core
} // namespace Acheron
