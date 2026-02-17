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
        // channel_id -> list of [start, end] range pairs for member list subscriptions
        QMap<Core::Snowflake, QList<QPair<int, int>>> channels;

        QJsonObject toJson() const
        {
            QJsonObject obj;
            insert(obj, "typing", typing);
            insert(obj, "activities", activities);
            insert(obj, "threads", threads);

            if (!channels.isEmpty()) {
                QJsonObject channelsObj;
                for (auto it = channels.begin(); it != channels.end(); ++it) {
                    QJsonArray rangesArr;
                    for (const auto &range : it.value()) {
                        QJsonArray pair;
                        pair.append(range.first);
                        pair.append(range.second);
                        rangesArr.append(pair);
                    }
                    channelsObj[QString::number(it.key())] = rangesArr;
                }
                obj["channels"] = channelsObj;
            }

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
using GuildSubscriptionsBulk =
        Outbound<OpCode::GUILD_SUBSCRIPTIONS_BULK, GuildSubscriptionsBulkData>;

struct RequestGuildMembersData : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<QList<Core::Snowflake>, true> userIds;
    Field<bool, true> presences;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        insert(obj, "guild_id", guildId);
        insert(obj, "user_ids", userIds);
        insert(obj, "presences", presences);
        return obj;
    }
};
using RequestGuildMembers = Outbound<OpCode::REQUEST_GUILD_MEMBERS, RequestGuildMembersData>;

struct ResumeData : Core::JsonUtils::JsonObject
{
    Field<QString> token;
    Field<QString> sessionId;
    Field<int, false, true> seq;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        insert(obj, "token", token);
        insert(obj, "session_id", sessionId);
        insert(obj, "seq", seq);
        return obj;
    }
};
using Resume = Outbound<OpCode::RESUME, ResumeData>;

struct UpdateVoiceStateData : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<Core::Snowflake, false, true> channelId;
    Field<bool> selfMute;
    Field<bool> selfDeaf;
    Field<bool, true> selfVideo;
    Field<VoiceFlags, true> flags;

    QJsonObject toJson() const
    {
        QJsonObject obj;
        obj["guild_id"] = QString::number(guildId.get());
        if (channelId.isNull())
            obj["channel_id"] = QJsonValue::Null;
        else
            obj["channel_id"] = QString::number(channelId.get());
        insert(obj, "self_mute", selfMute);
        insert(obj, "self_deaf", selfDeaf);
        insert(obj, "self_video", selfVideo);
        insert(obj, "flags", flags);
        return obj;
    }
};
using UpdateVoiceState = Outbound<OpCode::VOICE_STATE_UPDATE, UpdateVoiceStateData>;

} // namespace Discord
} // namespace Acheron
