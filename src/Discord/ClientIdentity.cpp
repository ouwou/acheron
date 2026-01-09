#include "ClientIdentity.hpp"
#include "CurlUtils.hpp"

#include <QMutexLocker>
#include <QUuid>

namespace Acheron {
namespace Discord {

ClientIdentity::ClientIdentity()
{
    launchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    launchSignature = generateLaunchSignature();
    regenerateClientHeartbeatSessionId();
}

void ClientIdentity::regenerateClientHeartbeatSessionId()
{
    QMutexLocker locker(&mutex);
    clientHeartbeatSessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString ClientIdentity::generateLaunchSignature()
{
    QUuid uuid = QUuid::createUuid();
    QByteArray bytes = uuid.toRfc4122();

    static constexpr quint8 mask[16] = {
        0xff, 0x7f, 0xef, 0xef, 0xf7, 0xef, 0xf7, 0xff,
        0xdf, 0x7e, 0xff, 0xbf, 0xfe, 0xff, 0xf7, 0xff,
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
    properties.systemLocale = "en-US";
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
    properties.clientAppState = params.clientAppState;

    if (params.isFastConnect.has_value())
        properties.isFastConnect = params.isFastConnect.value();

    if (params.gatewayConnectReasons.has_value())
        properties.gatewayConnectReasons = params.gatewayConnectReasons.value();

    if (params.includeClientHeartbeatSessionId)
        properties.clientHeartbeatSessionId = clientHeartbeatSessionId;

    return properties;
}

} // namespace Discord
} // namespace Acheron
