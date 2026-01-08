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
        if (globalName.hasValue())
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
        return member;
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
        return att;
    }

    bool isImage() const
    {
        if (!contentType.hasValue())
            return false;
        return contentType->startsWith("image/");
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
    Field<QList<Attachment>, true> attachments;
    Field<QList<Embed>, true> embeds;

    // cached data
    QString parsedContentCached;
    QString embedsJson;

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
        get(obj, "attachments", message.attachments);
        get(obj, "embeds", message.embeds);

        if (obj.contains("embeds")) {
            QJsonDocument doc(obj.value("embeds").toArray());
            message.embedsJson = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
        }

        return message;
    }
};

} // namespace Discord
} // namespace Acheron
