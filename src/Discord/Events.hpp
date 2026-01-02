#pragma once

#include <QString>

#include "Core/JsonUtils.hpp"
#include "Core/Snowflake.hpp"
#include "Entities.hpp"

namespace Acheron {
namespace Discord {

struct Ready : Core::JsonUtils::JsonObject
{
    Field<User> user;
    Field<QList<GatewayGuild>> guilds;

    static Ready fromJson(const QJsonObject &obj)
    {
        Ready ready;
        get(obj, "user", ready.user);
        get(obj, "guilds", ready.guilds);
        return ready;
    }
};

} // namespace Discord
} // namespace Acheron
