#pragma once

#include <QHash>
#include <QList>
#include <QObject>

#include <optional>

#include "Core/Snowflake.hpp"
#include "Discord/Events.hpp"
#include "Discord/StreamKey.hpp"

namespace Acheron {
namespace Core {

struct StreamRecord
{
    enum class State {
        AwaitingEndpoint,
        Ready,
        Reconnecting,
    };

    Discord::StreamKey key;
    State state = State::AwaitingEndpoint;
    QString region;
    QList<Snowflake> viewerIds;
    bool paused = false;

    Snowflake rtcServerId;
    Snowflake rtcChannelId;
    QString endpoint;
    QString token;
};

enum class StreamEndReason {
    UserRequested,
    StreamFull,
    Unauthorized,
    SafetyGuildRateLimited,
    Other,
};

[[nodiscard]] StreamEndReason streamEndReasonFromString(const QString &reason);
[[nodiscard]] inline bool isRefusal(StreamEndReason reason)
{
    return reason == StreamEndReason::Unauthorized || reason == StreamEndReason::SafetyGuildRateLimited;
}

// the gateway sends stream events only to the owner and viewers; who else is live comes from voice
// states
class StreamManager : public QObject
{
    Q_OBJECT
public:
    explicit StreamManager(QObject *parent = nullptr);

    void handleStreamCreate(const Discord::StreamCreate &event);
    void handleStreamServerUpdate(const Discord::StreamServerUpdate &event);
    void handleStreamUpdate(const Discord::StreamUpdate &event);
    void handleStreamDelete(const Discord::StreamDelete &event);

    void reset();

    [[nodiscard]] std::optional<StreamRecord> stream(const QString &streamKey) const;
    [[nodiscard]] QStringList streamKeys() const;

signals:
    void streamChanged(const QString &streamKey);
    void streamEnded(const QString &streamKey, Acheron::Core::StreamEndReason reason);

private:
    QHash<QString, StreamRecord> streams;
};

} // namespace Core
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Core::StreamEndReason)
