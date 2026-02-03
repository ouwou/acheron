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
    Field<QString> userSettingsProto;
    Field<QList<QList<Member>>, true> mergedMembers;
    Field<QList<User>, true> users;
    Field<QList<Channel>, true> privateChannels;

    static Ready fromJson(const QJsonObject &obj)
    {
        Ready ready;
        get(obj, "user", ready.user);
        get(obj, "guilds", ready.guilds);
        get(obj, "user_settings_proto", ready.userSettingsProto);
        get(obj, "merged_members", ready.mergedMembers);
        get(obj, "users", ready.users);
        get(obj, "private_channels", ready.privateChannels);
        return ready;
    }
};

struct SupplementalGuild : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;

    static SupplementalGuild fromJson(const QJsonObject &obj)
    {
        SupplementalGuild supplementalGuild;
        get(obj, "id", supplementalGuild.id);
        return supplementalGuild;
    }
};

struct ReadySupplemental : Core::JsonUtils::JsonObject
{
    Field<QList<SupplementalGuild>> guilds;
    Field<QList<QList<Member>>> mergedMembers;

    static ReadySupplemental fromJson(const QJsonObject &obj)
    {
        ReadySupplemental readySupplemental;
        get(obj, "guilds", readySupplemental.guilds);
        get(obj, "merged_members", readySupplemental.mergedMembers);
        return readySupplemental;
    }
};

struct TypingStart : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake, true> guildId;
    Field<Core::Snowflake> userId;
    Field<QDateTime> timestamp;
    Field<Member, true> member;

    static TypingStart fromJson(const QJsonObject &obj)
    {
        TypingStart typingStart;
        get(obj, "channel_id", typingStart.channelId);
        get(obj, "guild_id", typingStart.guildId);
        get(obj, "user_id", typingStart.userId);
        get(obj, "member", typingStart.member);
        typingStart.timestamp = QDateTime::fromSecsSinceEpoch(obj["timestamp"].toInteger());
        return typingStart;
    }
};

struct ChannelCreate : Core::JsonUtils::JsonObject
{
    Field<Channel> channel;

    static ChannelCreate fromJson(const QJsonObject &obj)
    {
        ChannelCreate event;
        event.channel = Channel::fromJson(obj);
        return event;
    }
};

struct ChannelUpdate : Core::JsonUtils::JsonObject
{
    Field<Channel> channel;

    static ChannelUpdate fromJson(const QJsonObject &obj)
    {
        ChannelUpdate update;
        update.channel = Channel::fromJson(obj);
        return update;
    }
};

struct ChannelDelete : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<Core::Snowflake, true> guildId;

    static ChannelDelete fromJson(const QJsonObject &obj)
    {
        ChannelDelete event;
        get(obj, "id", event.id);
        get(obj, "guild_id", event.guildId);
        return event;
    }
};

struct GuildMembersChunk : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<QList<Member>> members;
    Field<int> chunkIndex;
    Field<int> chunkCount;
    Field<QList<Core::Snowflake>, true> notFound;

    static GuildMembersChunk fromJson(const QJsonObject &obj)
    {
        GuildMembersChunk chunk;
        get(obj, "guild_id", chunk.guildId);
        get(obj, "members", chunk.members);
        get(obj, "chunk_index", chunk.chunkIndex);
        get(obj, "chunk_count", chunk.chunkCount);
        get(obj, "not_found", chunk.notFound);
        return chunk;
    }
};

struct MessageDelete : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake, true> guildId;

    static MessageDelete fromJson(const QJsonObject &obj)
    {
        MessageDelete event;
        get(obj, "id", event.id);
        get(obj, "channel_id", event.channelId);
        get(obj, "guild_id", event.guildId);
        return event;
    }
};

} // namespace Discord
} // namespace Acheron
