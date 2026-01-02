#pragma once

#include <QString>
#include <QVariantMap>
#include <QJsonObject>

#include <optional>

#include "Core/JsonUtils.hpp"

#ifdef compress // really bruh
#  undef compress
#endif

namespace Acheron {
namespace Discord {

struct ClientProperties : Acheron::Core::JsonUtils::JsonObject
{
    Field<QString> os;
    Field<QString> browser;
    Field<QString> device;
    Field<QString> systemLocale;
    Field<bool> hasClientMods;
    Field<QString> browserUserAgent;
    Field<QString> browserVersion;
    Field<QString> osVersion;
    Field<QString> referrer;
    Field<QString> referringDomain;
    Field<QString> referrerCurrent;
    Field<QString> referringDomainCurrent;
    Field<QString> releaseChannel;
    Field<int> clientBuildNumber;
    Field<QString, false, true> clientEventSource;
    Field<QString> clientLaunchId;
    Field<QString> launchSignature;
    Field<QString> clientAppState;
    Field<bool> isFastConnect;
    Field<QString> gatewayConnectReasons;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        insert(obj, "os", os);
        insert(obj, "browser", browser);
        insert(obj, "device", device);
        insert(obj, "system_locale", systemLocale);
        insert(obj, "has_client_mods", hasClientMods);
        insert(obj, "browser_user_agent", browserUserAgent);
        insert(obj, "browser_version", browserVersion);
        insert(obj, "os_version", osVersion);
        insert(obj, "referrer", referrer);
        insert(obj, "referring_domain", referringDomain);
        insert(obj, "referrer_current", referrerCurrent);
        insert(obj, "referring_domain_current", referringDomainCurrent);
        insert(obj, "release_channel", releaseChannel);
        insert(obj, "client_build_number", clientBuildNumber);
        insert(obj, "client_event_source", clientEventSource);
        insert(obj, "client_launch_id", clientLaunchId);
        insert(obj, "launch_signature", launchSignature);
        insert(obj, "client_app_state", clientAppState);
        insert(obj, "is_fast_connect", isFastConnect);
        insert(obj, "gateway_connect_reasons", gatewayConnectReasons);

        return obj;
    }
};

struct UpdatePresence : Acheron::Core::JsonUtils::JsonObject
{
    Field<QString> status;
    Field<int> since;
    // QList<Activity> activities;
    Field<bool> afk;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        insert(obj, "status", status);
        insert(obj, "since", since);
        insert(obj, "afk", afk);
        return obj;
    }
};

struct ClientState : Acheron::Core::JsonUtils::JsonObject
{
    // Map guild_versions;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        obj["guild_versions"] = QJsonObject();
        return obj;
    }
};

} // namespace Discord
} // namespace Acheron
