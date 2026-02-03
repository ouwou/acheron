#pragma once

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>

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
        if (globalName.hasValue() && !globalName->isEmpty())
            return globalName;
        return username;
    }
};

struct Member : Core::JsonUtils::JsonObject
{
    Field<User, true> user;
    Field<QString, true, true> nick;
    Field<QString, true, true> avatar;
    Field<QList<Core::Snowflake>, true> roles;
    Field<QDateTime, true> joinedAt;
    Field<QDateTime, true, true> premiumSince;
    Field<bool, true> deaf;
    Field<bool, true> mute;
    Field<int, true> flags;
    Field<bool, true> pending;
    Field<QDateTime, true, true> communicationDisabledUntil;
    Field<Core::Snowflake, true> userId; // supplemental

    static Member fromJson(const QJsonObject &obj)
    {
        Member member;
        get(obj, "user", member.user);
        get(obj, "nick", member.nick);
        get(obj, "avatar", member.avatar);
        get(obj, "roles", member.roles);
        get(obj, "joined_at", member.joinedAt);
        get(obj, "premium_since", member.premiumSince);
        get(obj, "deaf", member.deaf);
        get(obj, "mute", member.mute);
        get(obj, "flags", member.flags);
        get(obj, "pending", member.pending);
        get(obj, "communication_disabled_until", member.communicationDisabledUntil);
        get(obj, "user_id", member.userId);
        return member;
    }
};

struct Role : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<QString> name;
    Field<Permissions> permissions;
    Field<int> position;
    Field<int, true> color;
    Field<bool, true> hoist;
    Field<QString, true, true> icon;
    Field<QString, true, true> unicodeEmoji;
    Field<bool, true> managed;
    Field<bool, true> mentionable;

    static Role fromJson(const QJsonObject &obj)
    {
        Role role;
        get(obj, "id", role.id);
        get(obj, "name", role.name);
        if (obj.contains("permissions")) {
            QString permStr = obj["permissions"].toString();
            role.permissions = Permissions::fromInt(permStr.toULongLong());
        }
        get(obj, "position", role.position);
        get(obj, "color", role.color);
        get(obj, "hoist", role.hoist);
        get(obj, "icon", role.icon);
        get(obj, "unicode_emoji", role.unicodeEmoji);
        get(obj, "managed", role.managed);
        get(obj, "mentionable", role.mentionable);
        return role;
    }
};

struct PermissionOverwrite : Core::JsonUtils::JsonObject
{
    enum class Type {
        Role = 0,
        Member = 1,
    };

    Field<Core::Snowflake> id;
    Field<Type> type;
    Field<Permissions> allow;
    Field<Permissions> deny;

    static PermissionOverwrite fromJson(const QJsonObject &obj)
    {
        PermissionOverwrite overwrite;
        get(obj, "id", overwrite.id);
        if (obj.contains("type"))
            overwrite.type = static_cast<Type>(obj["type"].toInt());
        if (obj.contains("allow")) {
            QString allowStr = obj["allow"].toString();
            overwrite.allow = Permissions::fromInt(allowStr.toULongLong());
        }
        if (obj.contains("deny")) {
            QString denyStr = obj["deny"].toString();
            overwrite.deny = Permissions::fromInt(denyStr.toULongLong());
        }
        return overwrite;
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
    Field<QList<PermissionOverwrite>, true> permissionOverwrites;
    Field<QList<User>, true> recipients;
    Field<QList<Core::Snowflake>, true> recipientIds;
    Field<Core::Snowflake, true, true> lastMessageId;
    Field<QString, true, true> icon;
    Field<Core::Snowflake, true> ownerId;

    static Channel fromJson(const QJsonObject &obj)
    {
        Channel channel;
        get(obj, "id", channel.id);
        get(obj, "type", channel.type);
        get(obj, "name", channel.name);
        get(obj, "position", channel.position);
        get(obj, "guild_id", channel.guildId);
        get(obj, "parent_id", channel.parentId);
        get(obj, "permission_overwrites", channel.permissionOverwrites);
        get(obj, "recipients", channel.recipients);
        get(obj, "recipient_ids", channel.recipientIds);
        get(obj, "last_message_id", channel.lastMessageId);
        get(obj, "icon", channel.icon);
        get(obj, "owner_id", channel.ownerId);
        return channel;
    }
};

struct Guild : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<QString> name;
    Field<QString> icon;
    Field<Core::Snowflake> ownerId;
    Field<QList<Role>, true> roles;

    static Guild fromJson(const QJsonObject &obj)
    {
        Guild guild;
        get(obj, "id", guild.id);
        get(obj, "name", guild.name);
        get(obj, "icon", guild.icon);
        get(obj, "owner_id", guild.ownerId);
        get(obj, "roles", guild.roles);
        return guild;
    }
};

struct GatewayGuild : Core::JsonUtils::JsonObject
{
    Field<Guild> properties;
    Field<QList<Channel>> channels;
    Field<QList<Role>, true> roles;

    static GatewayGuild fromJson(const QJsonObject &obj)
    {
        GatewayGuild guild;
        get(obj, "properties", guild.properties);
        get(obj, "channels", guild.channels);
        get(obj, "roles", guild.roles);
        return guild;
    }

    Guild asGuild() const
    {
        Guild guild = properties;
        guild.roles = roles;
        return guild;
    }
};

struct EmbedFooter : Core::JsonUtils::JsonObject
{
    Field<QString> text;
    Field<QString, true> iconUrl;
    Field<QString, true> proxyIconUrl;

    static EmbedFooter fromJson(const QJsonObject &obj)
    {
        EmbedFooter footer;
        get(obj, "text", footer.text);
        get(obj, "icon_url", footer.iconUrl);
        get(obj, "proxy_icon_url", footer.proxyIconUrl);
        return footer;
    }
};

struct EmbedMedia : Core::JsonUtils::JsonObject
{
    Field<QString> url;
    Field<QString, true> proxyUrl;
    Field<int, true> width;
    Field<int, true> height;
    Field<QString, true> contentType;

    static EmbedMedia fromJson(const QJsonObject &obj)
    {
        EmbedMedia image;
        get(obj, "url", image.url);
        get(obj, "proxy_url", image.proxyUrl);
        get(obj, "width", image.width);
        get(obj, "height", image.height);
        get(obj, "content_type", image.contentType);
        return image;
    }
};

struct EmbedProvider : Core::JsonUtils::JsonObject
{
    Field<QString, true> name;
    Field<QString, true> url;

    static EmbedProvider fromJson(const QJsonObject &obj)
    {
        EmbedProvider provider;
        get(obj, "name", provider.name);
        get(obj, "url", provider.url);
        return provider;
    }
};

struct EmbedAuthor : Core::JsonUtils::JsonObject
{
    Field<QString> name;
    Field<QString, true> url;
    Field<QString, true> iconUrl;
    Field<QString, true> proxyIconUrl;

    static EmbedAuthor fromJson(const QJsonObject &obj)
    {
        EmbedAuthor author;
        get(obj, "name", author.name);
        get(obj, "url", author.url);
        get(obj, "icon_url", author.iconUrl);
        get(obj, "proxy_icon_url", author.proxyIconUrl);
        return author;
    }
};

struct EmbedField : Core::JsonUtils::JsonObject
{
    Field<QString> name;
    Field<QString> value;
    Field<bool, true> isInline;

    static EmbedField fromJson(const QJsonObject &obj)
    {
        EmbedField field;
        get(obj, "name", field.name);
        get(obj, "value", field.value);
        get(obj, "inline", field.isInline);
        return field;
    }
};

struct Embed : Core::JsonUtils::JsonObject
{
    Field<QString, true> title;
    Field<QString, true> type;
    Field<QString, true> description;
    Field<QString, true> url;
    Field<QDateTime, true> timestamp;
    Field<int, true> color;
    Field<EmbedFooter, true> footer;
    Field<EmbedMedia, true> image;
    Field<EmbedMedia, true> thumbnail;
    Field<EmbedMedia, true> video;
    Field<EmbedProvider, true> provider;
    Field<EmbedAuthor, true> author;
    Field<QList<EmbedField>, true> fields;

    static Embed fromJson(const QJsonObject &obj)
    {
        Embed embed;
        get(obj, "title", embed.title);
        get(obj, "type", embed.type);
        get(obj, "description", embed.description);
        get(obj, "url", embed.url);
        get(obj, "timestamp", embed.timestamp);
        get(obj, "color", embed.color);
        get(obj, "footer", embed.footer);
        get(obj, "image", embed.image);
        get(obj, "thumbnail", embed.thumbnail);
        get(obj, "video", embed.video);
        get(obj, "provider", embed.provider);
        get(obj, "author", embed.author);
        get(obj, "fields", embed.fields);
        return embed;
    }
};

struct Attachment : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<QString> filename;
    Field<QString, true> contentType;
    Field<qint64> size;
    Field<QString> url;
    Field<QString> proxyUrl;
    Field<int, true> width;
    Field<int, true> height;
    Field<AttachmentFlags, true> flags;

    static Attachment fromJson(const QJsonObject &obj)
    {
        Attachment att;
        get(obj, "id", att.id);
        get(obj, "filename", att.filename);
        get(obj, "content_type", att.contentType);
        get(obj, "size", att.size);
        get(obj, "url", att.url);
        get(obj, "proxy_url", att.proxyUrl);
        get(obj, "width", att.width);
        get(obj, "height", att.height);
        get(obj, "flags", att.flags);
        return att;
    }

    bool isImage() const
    {
        if (!contentType.hasValue())
            return false;
        return contentType->startsWith("image/");
    }

    bool isSpoiler() const
    {
        return flags.hasValue() && flags->testFlag(AttachmentFlag::IS_SPOILER);
    }
};

struct Message : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<QString, true> nonce;
    Field<Core::Snowflake> channelId;
    Field<User> author;
    Field<QString> content;
    Field<QDateTime> timestamp;
    Field<QDateTime, false, true> editedTimestamp;
    Field<MessageType> type;
    Field<MessageFlags> flags;
    Field<QList<Attachment>, true> attachments;
    Field<QList<Embed>, true> embeds;

    // TRANSIENT for MESSAGE_UPDATE
    Field<Core::Snowflake, true> guildId;
    Field<ChannelType, true> channelType;

    // cached data
    QString parsedContentCached;
    QString embedsJson;

    // sent
    bool isPendingOutbound = false;

    static Message fromJson(const QJsonObject &obj)
    {
        Message message;
        get(obj, "id", message.id);
        get(obj, "nonce", message.nonce);
        get(obj, "channel_id", message.channelId);
        get(obj, "author", message.author);
        get(obj, "content", message.content);
        get(obj, "timestamp", message.timestamp);
        get(obj, "edited_timestamp", message.editedTimestamp);
        get(obj, "type", message.type);
        get(obj, "flags", message.flags);
        get(obj, "attachments", message.attachments);
        get(obj, "embeds", message.embeds);
        get(obj, "guild_id", message.guildId);
        get(obj, "channel_type", message.channelType);

        if (obj.contains("embeds")) {
            QJsonDocument doc(obj.value("embeds").toArray());
            message.embedsJson = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
        }

        return message;
    }
};

} // namespace Discord
} // namespace Acheron
