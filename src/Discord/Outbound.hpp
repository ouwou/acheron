#pragma once

#include "Objects.hpp"

namespace Acheron {
namespace Discord {

template <OpCode op, typename T>
struct Outbound : public T
{
    static constexpr OpCode opcode = op;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        obj.insert("op", static_cast<int>(opcode));
        obj.insert("d", T::toJson());
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

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
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

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "active", active);
        insert(obj, "ver", ver);
        insert(obj, "reasons", reasons);
        return obj;
    }
};

struct QoSHeartbeatData : Core::JsonUtils::JsonObject
{
    Field<int, false, true> seq;
    Field<QoSPayload> qos;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "seq", seq);
        insert(obj, "qos", qos);
        return obj;
    }
};
using QoSHeartbeat = Outbound<OpCode::QOS_HEARTBEAT, QoSHeartbeatData>;

struct UpdateTimeSpentSessionIdData : Core::JsonUtils::JsonObject
{
    Field<qint64> initializationTimestamp;
    Field<QString> sessionId;
    Field<QString> clientLaunchId;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "initialization_timestamp", initializationTimestamp);
        insert(obj, "session_id", sessionId);
        insert(obj, "client_launch_id", clientLaunchId);
        return obj;
    }
};
using UpdateTimeSpentSessionId = Outbound<OpCode::UPDATE_TIME_SPENT_SESSION_ID, UpdateTimeSpentSessionIdData>;

struct GuildSubscriptionsBulkData : Core::JsonUtils::JsonObject
{
    struct SubscriptionData : Core::JsonUtils::JsonObject
    {
        Field<bool, true> typing;
        Field<bool, true> activities;
        Field<bool, true> threads;
        Field<bool, true> memberUpdates;
        // channel_id -> list of [start, end] range pairs for member list subscriptions
        QMap<Core::Snowflake, QList<QPair<int, int>>> channels;

        Core::OrderedJson toJson() const
        {
            Core::OrderedJson obj;
            insert(obj, "typing", typing);
            insert(obj, "activities", activities);
            insert(obj, "threads", threads);
            insert(obj, "member_updates", memberUpdates);

            if (!channels.isEmpty()) {
                Core::OrderedJson channelsObj;
                for (auto it = channels.begin(); it != channels.end(); ++it) {
                    Core::OrderedJson::Array ranges;
                    for (const auto &range : it.value())
                        ranges.append(Core::OrderedJson::Array().append(range.first).append(range.second));
                    channelsObj.insert(QString::number(it.key()), ranges);
                }
                obj.insert("channels", channelsObj);
            }

            return obj;
        }
    };

    Field<QMap<Core::Snowflake, SubscriptionData>> subscriptions;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "subscriptions", subscriptions);
        return obj;
    }
};
using GuildSubscriptionsBulk =
        Outbound<OpCode::GUILD_SUBSCRIPTIONS_BULK, GuildSubscriptionsBulkData>;

struct RequestForumUnreadsData : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<Core::Snowflake> channelId; // the forum
    // (post id, message id or invalid if not acked)
    QList<QPair<Core::Snowflake, Core::Snowflake>> threads;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "guild_id", guildId);
        insert(obj, "channel_id", channelId);

        Core::OrderedJson::Array arr;
        for (const auto &[threadId, ackMessageId] : threads) {
            Core::OrderedJson t;
            t.insert("thread_id", QString::number(threadId));
            t.insert("ack_message_id", ackMessageId.isValid()
                                               ? QJsonValue(QString::number(ackMessageId))
                                               : QJsonValue(QJsonValue::Null));
            arr.append(t);
        }
        obj.insert("threads", arr);
        return obj;
    }
};
using RequestForumUnreads = Outbound<OpCode::REQUEST_FORUM_UNREADS, RequestForumUnreadsData>;

struct RequestGuildMembersData : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<QString, true> query;
    Field<int, true> limit;
    Field<QList<Core::Snowflake>, true> userIds;
    Field<bool, true> presences;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        obj.insert("guild_id", Core::OrderedJson::Array().append(QString::number(guildId.get())));
        insert(obj, "query", query);
        insert(obj, "limit", limit);
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

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "token", token);
        insert(obj, "session_id", sessionId);
        insert(obj, "seq", seq);
        return obj;
    }
};
using Resume = Outbound<OpCode::RESUME, ResumeData>;

struct UpdateVoiceStateData : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake, false, true> guildId;
    Field<Core::Snowflake, false, true> channelId;
    Field<bool> selfMute;
    Field<bool> selfDeaf;
    Field<bool, true> selfVideo;
    Field<VoiceFlags, true> flags;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "guild_id", guildId);
        insert(obj, "channel_id", channelId);
        insert(obj, "self_mute", selfMute);
        insert(obj, "self_deaf", selfDeaf);
        insert(obj, "self_video", selfVideo);
        insert(obj, "flags", flags);
        return obj;
    }
};
using UpdateVoiceState = Outbound<OpCode::VOICE_STATE_UPDATE, UpdateVoiceStateData>;

// an invalid guild is a private call and an invalid channel leaves voice; both go as null
inline UpdateVoiceState voiceStateUpdate(Core::Snowflake guildId, Core::Snowflake channelId, bool selfMute, bool selfDeaf, bool selfVideo)
{
    UpdateVoiceState update;
    if (guildId.isValid())
        update.guildId = guildId;
    else
        update.guildId = nullptr;
    if (channelId.isValid())
        update.channelId = channelId;
    else
        update.channelId = nullptr;
    update.selfMute = selfMute;
    update.selfDeaf = selfDeaf;
    update.selfVideo = selfVideo;
    return update;
}

struct ConnectCallData : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "channel_id", channelId);
        return obj;
    }
};
using ConnectCall = Outbound<OpCode::CALL_CONNECT, ConnectCallData>;

struct CreateStreamData : Core::JsonUtils::JsonObject
{
    Field<QString> type;
    Field<Core::Snowflake, false, true> guildId;
    Field<Core::Snowflake> channelId;
    Field<QString, false, true> preferredRegion;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "type", type);
        insert(obj, "guild_id", guildId);
        insert(obj, "channel_id", channelId);
        insert(obj, "preferred_region", preferredRegion);
        return obj;
    }
};
using CreateStream = Outbound<OpCode::STREAM_CREATE, CreateStreamData>;

struct StreamKeyData : Core::JsonUtils::JsonObject
{
    Field<QString> streamKey;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "stream_key", streamKey);
        return obj;
    }
};
using DeleteStream = Outbound<OpCode::STREAM_DELETE, StreamKeyData>;
using WatchStream = Outbound<OpCode::STREAM_WATCH, StreamKeyData>;
using PingStream = Outbound<OpCode::STREAM_PING, StreamKeyData>;

struct SetStreamPausedData : Core::JsonUtils::JsonObject
{
    Field<QString> streamKey;
    Field<bool> paused;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "stream_key", streamKey);
        insert(obj, "paused", paused);
        return obj;
    }
};
using SetStreamPaused = Outbound<OpCode::STREAM_SET_PAUSED, SetStreamPausedData>;

} // namespace Discord
} // namespace Acheron
