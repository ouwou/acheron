#include "ClientIdentity.hpp"
#include "CurlUtils.hpp"

#include <QDateTime>
#include <QMutexLocker>
#include <QUuid>

namespace Acheron {
namespace Discord {

namespace {
constexpr qint64 HeartbeatSessionIdleMs = 30 * 60 * 1000;

bool isExpired(const HeartbeatSession &session, qint64 nowMs)
{
    return nowMs - session.lastUsedAtMs >= HeartbeatSessionIdleMs;
}
} // namespace

ClientIdentity::ClientIdentity()
{
    launchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    launchSignature = generateLaunchSignature();
}

QString ClientIdentity::clientLaunchId() const
{
    return launchId;
}

std::optional<HeartbeatSession> ClientIdentity::heartbeatSession() const
{
    QMutexLocker locker(&mutex);
    if (!clientHeartbeatSession || isExpired(*clientHeartbeatSession, QDateTime::currentMSecsSinceEpoch()))
        return std::nullopt;
    return clientHeartbeatSession;
}

void ClientIdentity::restoreHeartbeatSession(const std::optional<HeartbeatSession> &stored)
{
    QMutexLocker locker(&mutex);
    clientHeartbeatSession = stored;
}

HeartbeatSessionUpdate ClientIdentity::touchHeartbeatSession()
{
    QMutexLocker locker(&mutex);
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool expired = !clientHeartbeatSession || isExpired(*clientHeartbeatSession, now);

    if (!appFocused && !rtcConnected) {
        if (expired)
            clientHeartbeatSession.reset();

        return HeartbeatSessionUpdate::Unchanged;
    }

    if (expired) {
        HeartbeatSession session;
        session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        session.createdAtMs = now;
        session.lastUsedAtMs = now;
        clientHeartbeatSession = session;
        return HeartbeatSessionUpdate::Created;
    }

    clientHeartbeatSession->lastUsedAtMs = now;
    return HeartbeatSessionUpdate::Touched;
}

void ClientIdentity::setDiscordLocale(const QString &newLocale)
{
    QMutexLocker locker(&mutex);
    if (!newLocale.isEmpty())
        locale = newLocale;
}

QString ClientIdentity::discordLocale() const
{
    QMutexLocker locker(&mutex);
    return locale;
}

void ClientIdentity::setActivity(bool focused, bool newRtcConnected)
{
    QMutexLocker locker(&mutex);
    appFocused = focused;
    rtcConnected = newRtcConnected;
}

QString ClientIdentity::generateLaunchSignature()
{
    QUuid uuid = QUuid::createUuid();
    QByteArray bytes = uuid.toRfc4122();

    static constexpr quint8 mask[16] = {
        0xff, 0x7f, 0xef, 0xef, 0xf7, 0xef, 0xf7, 0xff,
        0xdf, 0x7e, 0xff, 0xbf, 0xfe, 0xff, 0xf7, 0xff
    };

    for (int i = 0; i < 16; i++)
        bytes[i] = static_cast<char>(static_cast<quint8>(bytes[i]) & static_cast<quint8>(mask[i]));

    QUuid signature = QUuid::fromRfc4122(bytes);
    return signature.toString(QUuid::WithoutBraces);
}

ClientProperties ClientIdentity::buildClientProperties(
        const ClientPropertiesBuildParams &params) const
{
    QString userAgent = CurlUtils::getUserAgent();
    CurlUtils::UserAgentProps props = CurlUtils::getUserAgentProps();

    QMutexLocker locker(&mutex);

    ClientProperties properties;
    properties.os = props.os;
    properties.browser = props.browser;
    properties.device = "";
    properties.systemLocale = CurlUtils::getSystemLocale();
    properties.hasClientMods = false;
    properties.browserUserAgent = userAgent;
    properties.browserVersion = props.browserVersion;
    properties.osVersion = props.osVersion;
    properties.referrer = "";
    properties.referringDomain = "";
    properties.referrerCurrent = "";
    properties.referringDomainCurrent = "";
    properties.releaseChannel = "stable";
    properties.clientBuildNumber = CurlUtils::getBuildNumber();
    properties.clientEventSource = nullptr;
    properties.clientLaunchId = launchId;
    properties.launchSignature = launchSignature;
    properties.clientAppState = appFocused ? "focused" : "unfocused";

    if (params.isFastConnect.has_value())
        properties.isFastConnect = params.isFastConnect.value();

    if (params.gatewayConnectReasons.has_value())
        properties.gatewayConnectReasons = params.gatewayConnectReasons.value();

    if (params.includeClientHeartbeatSessionId &&
        clientHeartbeatSession &&
        !isExpired(*clientHeartbeatSession, QDateTime::currentMSecsSinceEpoch()))
        properties.clientHeartbeatSessionId = clientHeartbeatSession->id;

    return properties;
}

} // namespace Discord
} // namespace Acheron
