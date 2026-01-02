#pragma once

#include "Objects.hpp"

namespace Acheron {
namespace Discord {

template <OpCode op, typename T>
struct Outbound : public T
{
    static constexpr OpCode opcode = op;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        obj["op"] = static_cast<int>(opcode);
        obj["d"] = T::toJson();
        return obj;
    }
};

struct IdentifyData : Core::JsonUtils::JsonObject
{
    Field<QString> token;
    Field<Capabilities> capabilities;
    Field<ClientProperties> properties;
    Field<UpdatePresence> presence;
    Field<bool> compress;
    Field<ClientState> clientState;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        insert(obj, "token", token);
        insert(obj, "capabilities", capabilities);
        insert(obj, "properties", properties);
        insert(obj, "presence", presence);
        insert(obj, "compress", compress);
        insert(obj, "client_state", clientState);
        return obj;
    }
};
using Identify = Outbound<OpCode::IDENTIFY, IdentifyData>;

struct QoSPayload : Core::JsonUtils::JsonObject
{
    Field<int> ver;
    Field<bool> active;
    Field<QList<QString>> reasons;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        insert(obj, "ver", ver);
        insert(obj, "active", active);
        insert(obj, "reasons", reasons);
        return obj;
    }
};

struct QoSHeartbeatData : Core::JsonUtils::JsonObject
{
    Field<int, false, true> seq;
    Field<QoSPayload> qos;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        insert(obj, "seq", seq);
        insert(obj, "qos", qos);
        return obj;
    }
};
using QoSHeartbeat = Outbound<OpCode::QOS_HEARTBEAT, QoSHeartbeatData>;

struct GuildSubscriptionsBulkData : Core::JsonUtils::JsonObject
{
    struct SubscriptionData : Core::JsonUtils::JsonObject
    {
        Field<bool> typing;
        Field<bool> activities;
        Field<bool> threads;

        QJsonObject toJson() const
        {
            QJsonObject obj;
            insert(obj, "typing", typing);
            insert(obj, "activities", activities);
            insert(obj, "threads", threads);
            return obj;
        }
    };

    Field<QMap<Core::Snowflake, SubscriptionData>> subscriptions;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        insert(obj, "subscriptions", subscriptions);
        return obj;
    }
};
using GuildSubscriptionsBulk = Outbound<OpCode::GUILD_SUBSCRIPTIONS_BULK, GuildSubscriptionsBulkData>;

} // namespace Discord
} // namespace Acheron
