#pragma once

#include <QString>
#include <QJsonObject>

#include "Enums.hpp"

#include "Core/Snowflake.hpp"
#include "Core/JsonUtils.hpp"

namespace Acheron {
namespace Discord {

struct User : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<QString> username;
    Field<QString, false, true> globalName;
    Field<QString, false, true> avatar;
    Field<bool, true> bot;

    static User fromJson(const QJsonObject &obj)
    {
        User user;
        get(obj, "id", user.id);
        get(obj, "username", user.username);
        get(obj, "global_name", user.globalName);
        get(obj, "avatar", user.avatar);
        get(obj, "bot", user.bot);
        return user;
    }

    QString getDisplayName() const
    {
        if (globalName.hasValue())
            return globalName;
        return username;
    }
};

struct Channel : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<ChannelType> type;
    Field<QString, true, true> name;
    Field<int, true> position;
    Field<Core::Snowflake, true> guildId;
    Field<Core::Snowflake, true, true> parentId;

    static Channel fromJson(const QJsonObject &obj)
    {
        Channel channel;
        get(obj, "id", channel.id);
        get(obj, "type", channel.type);
        get(obj, "name", channel.name);
        get(obj, "position", channel.position);
        get(obj, "guild_id", channel.guildId);
        get(obj, "parent_id", channel.parentId);
        return channel;
    }
};

struct Guild : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<QString> name;
    Field<QString> icon;
    Field<Core::Snowflake> ownerId;

    static Guild fromJson(const QJsonObject &obj)
    {
        Guild guild;
        get(obj, "id", guild.id);
        get(obj, "name", guild.name);
        get(obj, "icon", guild.icon);
        get(obj, "owner_id", guild.ownerId);
        return guild;
    }
};

struct GatewayGuild : Core::JsonUtils::JsonObject
{
    Field<Guild> properties;
    Field<QList<Channel>> channels;

    static GatewayGuild fromJson(const QJsonObject &obj)
    {
        GatewayGuild guild;
        get(obj, "properties", guild.properties);
        get(obj, "channels", guild.channels);
        return guild;
    }

    Guild asGuild() const
    {
        Guild guild = properties;
        // todo copy other fields
        return guild;
    }
};

struct Message : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<Core::Snowflake> channelId;
    Field<User> author;
    Field<QString> content;
    Field<QDateTime> timestamp;
    Field<QDateTime, false, true> editedTimestamp;
    Field<MessageType> type;
    Field<MessageFlags> flags;

    // ui stuff
    QString parsedContentCached;

    static Message fromJson(const QJsonObject &obj)
    {
        Message message;
        get(obj, "id", message.id);
        get(obj, "channel_id", message.channelId);
        get(obj, "author", message.author);
        get(obj, "content", message.content);
        get(obj, "timestamp", message.timestamp);
        get(obj, "edited_timestamp", message.editedTimestamp);
        get(obj, "type", message.type);
        get(obj, "flags", message.flags);
        return message;
    }
};

} // namespace Discord
} // namespace Acheron
