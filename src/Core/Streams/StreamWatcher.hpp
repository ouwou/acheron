#pragma once

#include <QHash>
#include <QObject>
#include <QSet>

#include <optional>

#include "Core/GatewayNotice.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Streams/StreamManager.hpp"
#include "Core/Streams/StreamSession.hpp"
#include "Discord/StreamKey.hpp"

namespace Acheron {
namespace Discord {
class Client;
}

namespace Core {

namespace Audio {
class VoiceManager;
}

// follows the official client's rules for when watching one stream ends another
class StreamWatcher : public QObject
{
    Q_OBJECT
public:
    StreamWatcher(Discord::Client *client, StreamManager *streams, Audio::VoiceManager *voice, QObject *parent = nullptr);

    [[nodiscard]] bool canWatch(const Discord::StreamKey &key) const;
    void watch(const Discord::StreamKey &key);
    void stopWatching(const QString &streamKey);
    void stopAll();

    void reset();

    [[nodiscard]] StreamSession *session(const QString &streamKey) const;
    [[nodiscard]] QList<StreamSession *> openSessions() const { return sessions.values(); }

    void watchWhenConnected(const Discord::StreamKey &key);

signals:
    void sessionOpened(Acheron::Core::StreamSession *session);
    void sessionClosed(const QString &streamKey);

private slots:
    void onStreamChanged(const QString &streamKey);
    void onStreamEnded(const QString &streamKey, Acheron::Core::StreamEndReason reason);
    void onVoiceStateChanged();
    void onVoiceConnected();
    void onParticipantChanged(Snowflake userId);

private:
    void remove(const QString &streamKey, GatewayNotice notice);
    void autoWatchStreamOf(Snowflake userId);

    Discord::Client *client;
    StreamManager *streams;
    Audio::VoiceManager *voice;
    Snowflake accountId;

    QHash<QString, StreamSession *> sessions;
    std::optional<Discord::StreamKey> pendingWatch;
    QSet<QString> fullStreams;
    QString lastAutoWatched;
    quint32 nextAudioSource = 1;
};

} // namespace Core
} // namespace Acheron
