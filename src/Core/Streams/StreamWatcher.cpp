#include "StreamWatcher.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Discord/Client.hpp"

namespace Acheron {
namespace Core {

StreamWatcher::StreamWatcher(Discord::Client *client, StreamManager *streams, Audio::VoiceManager *voice, QObject *parent)
    : QObject(parent), client(client), streams(streams), voice(voice), accountId(voice->accountUserId())
{
    connect(streams, &StreamManager::streamChanged, this, &StreamWatcher::onStreamChanged);
    connect(streams, &StreamManager::streamEnded, this, &StreamWatcher::onStreamEnded);
    connect(voice, &Audio::VoiceManager::voiceStateChanged, this, &StreamWatcher::onVoiceStateChanged);
    connect(voice, &Audio::VoiceManager::voiceDisconnected, this, &StreamWatcher::stopAll);
    connect(voice, &Audio::VoiceManager::voiceConnected, this, &StreamWatcher::onVoiceConnected);
    connect(voice, &Audio::VoiceManager::participantJoined, this, &StreamWatcher::onParticipantChanged);
    connect(voice, &Audio::VoiceManager::participantUpdated, this, &StreamWatcher::onParticipantChanged);
}

void StreamWatcher::watchWhenConnected(const Discord::StreamKey &key)
{
    if (canWatch(key)) {
        watch(key);
        return;
    }
    if (key.ownerId != accountId)
        pendingWatch = key;
}

// in a private call the official client watches unasked: the first live stream on joining, then
// whoever's voice state last arrived saying they are live (130553.js)
void StreamWatcher::onVoiceConnected()
{
    lastAutoWatched.clear();

    const QList<Audio::VoiceParticipant> participants = voice->currentParticipants();
    for (const Audio::VoiceParticipant &participant : participants) {
        const QString streamKey = voice->streamKeyOf(participant.userId).toString();
        if (participant.userId == accountId || !participant.selfStream || fullStreams.contains(streamKey))
            continue;
        autoWatchStreamOf(participant.userId);
        return;
    }
}

void StreamWatcher::onParticipantChanged(Snowflake userId)
{
    const Audio::VoiceParticipant *participant = voice->participant(userId);
    if (!participant || userId == accountId)
        return;

    if (participant->selfStream)
        autoWatchStreamOf(userId);
    else
        fullStreams.remove(voice->streamKeyOf(userId).toString());
}

void StreamWatcher::autoWatchStreamOf(Snowflake userId)
{
    const bool inPrivateCall = voice->isConnected() && !voice->currentGuildId().isValid();
    if (!inPrivateCall)
        return;

    const Discord::StreamKey key = voice->streamKeyOf(userId);
    const QString streamKey = key.toString();
    if (sessions.contains(streamKey) || streamKey == lastAutoWatched || fullStreams.contains(streamKey))
        return;

    lastAutoWatched = streamKey;
    watch(key);
}

bool StreamWatcher::canWatch(const Discord::StreamKey &key) const
{
    return key.ownerId != accountId && voice->isConnected() && voice->currentChannelId() == key.channelId;
}

StreamSession *StreamWatcher::session(const QString &streamKey) const
{
    return sessions.value(streamKey, nullptr);
}

void StreamWatcher::watch(const Discord::StreamKey &key)
{
    if (!canWatch(key))
        return;

    const QString streamKey = key.toString();
    if (StreamSession *existing = sessions.value(streamKey, nullptr)) {
        if (!existing->isOver()) {
            emit sessionOpened(existing);
            return;
        }
        remove(streamKey, GatewayNotice::Skip);
    }

    // as the official client
    if (sessions.size() < 2) {
        const QStringList others = sessions.keys();
        for (const QString &other : others)
            remove(other, GatewayNotice::Send);
    }

    auto *session = new StreamSession(key, voice, nextAudioSource++, this);
    connect(session, &StreamSession::pingRequested, this, [this, streamKey] { client->sendStreamPing(streamKey); });
    sessions.insert(streamKey, session);

    client->sendStreamWatch(streamKey);
    emit sessionOpened(session);
}

void StreamWatcher::stopWatching(const QString &streamKey)
{
    remove(streamKey, GatewayNotice::Send);
}

void StreamWatcher::stopAll()
{
    const QStringList keys = sessions.keys();
    for (const QString &key : keys)
        remove(key, GatewayNotice::Send);
}

void StreamWatcher::reset()
{
    pendingWatch.reset();
    fullStreams.clear();
    const QStringList keys = sessions.keys();
    for (const QString &key : keys)
        remove(key, GatewayNotice::Skip);
}

void StreamWatcher::remove(const QString &streamKey, GatewayNotice notice)
{
    StreamSession *session = sessions.take(streamKey);
    if (!session)
        return;

    if (notice == GatewayNotice::Send && !session->isOver())
        client->sendStreamDelete(streamKey);

    emit sessionClosed(streamKey);
    delete session;
}

void StreamWatcher::onStreamChanged(const QString &streamKey)
{
    StreamSession *session = sessions.value(streamKey, nullptr);
    const std::optional<StreamRecord> record = streams->stream(streamKey);
    if (session && record)
        session->apply(*record);
}

void StreamWatcher::onStreamEnded(const QString &streamKey, StreamEndReason reason)
{
    if (reason == StreamEndReason::StreamFull)
        fullStreams.insert(streamKey);
    if (StreamSession *session = sessions.value(streamKey, nullptr))
        session->end(reason);
}

void StreamWatcher::onVoiceStateChanged()
{
    const QStringList keys = sessions.keys();
    for (const QString &key : keys) {
        StreamSession *session = sessions.value(key, nullptr);
        if (session && voice->currentChannelId() != session->key().channelId)
            remove(key, GatewayNotice::Send);
    }

    if (pendingWatch && canWatch(*pendingWatch)) {
        const Discord::StreamKey key = *pendingWatch;
        pendingWatch.reset();
        watch(key);
    } else if (pendingWatch && voice->isConnected() && voice->currentChannelId() != pendingWatch->channelId) {
        pendingWatch.reset();
    }
}

} // namespace Core
} // namespace Acheron
