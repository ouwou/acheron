#pragma once

#include <QString>
#include <QMutex>

#include <optional>

#include "Objects.hpp"

namespace Acheron {
namespace Discord {

struct ClientPropertiesBuildParams
{
    bool includeClientHeartbeatSessionId;
    std::optional<bool> isFastConnect;
    std::optional<QString> gatewayConnectReasons;
};

struct HeartbeatSession
{
    QString id;
    qint64 createdAtMs = 0;
    qint64 lastUsedAtMs = 0;
};

enum class HeartbeatSessionUpdate {
    Unchanged,
    Touched,
    Created,
};

class ClientIdentity
{
public:
    ClientIdentity();

    QString clientLaunchId() const;

    std::optional<HeartbeatSession> heartbeatSession() const;
    void restoreHeartbeatSession(const std::optional<HeartbeatSession> &stored);
    HeartbeatSessionUpdate touchHeartbeatSession();

    void setDiscordLocale(const QString &locale);
    QString discordLocale() const;

    void setActivity(bool focused, bool rtcConnected);

    ClientProperties buildClientProperties(const ClientPropertiesBuildParams &params) const;

private:
    static QString generateLaunchSignature();

    mutable QMutex mutex;
    QString launchId;
    QString launchSignature;
    std::optional<HeartbeatSession> clientHeartbeatSession;
    QString locale = "en-US";
    bool appFocused = true;
    bool rtcConnected = false;
};

} // namespace Discord
} // namespace Acheron
