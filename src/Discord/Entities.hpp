#pragma once

#include <memory>

#include <QString>
#include <QColor>
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

    bool hasColor() const
    {
        return color.hasValue() && color.get() != 0;
    }

    QColor getColor() const
    {
        return hasColor() ? QColor::fromRgb(color.get()) : QColor();
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
    Field<int, true> rateLimitPerUser;

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
        get(obj, "rate_limit_per_user", channel.rateLimitPerUser);
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

struct VoiceState : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake, true> guildId;
    Field<Core::Snowflake, false, true> channelId;
    Field<Core::Snowflake> userId;
    Field<QString> sessionId;
    Field<bool> deaf;
    Field<bool> mute;
    Field<bool> selfDeaf;
    Field<bool> selfMute;
    Field<bool, true> selfStream;
    Field<bool> selfVideo;
    Field<bool> suppress;
    Field<QString, true> requestToSpeakTimestamp;

    static VoiceState fromJson(const QJsonObject &obj)
    {
        VoiceState state;
        get(obj, "guild_id", state.guildId);
        get(obj, "channel_id", state.channelId);
        get(obj, "user_id", state.userId);
        get(obj, "session_id", state.sessionId);
        get(obj, "deaf", state.deaf);
        get(obj, "mute", state.mute);
        get(obj, "self_deaf", state.selfDeaf);
        get(obj, "self_mute", state.selfMute);
        get(obj, "self_stream", state.selfStream);
        get(obj, "self_video", state.selfVideo);
        get(obj, "suppress", state.suppress);
        get(obj, "request_to_speak_timestamp", state.requestToSpeakTimestamp);
        return state;
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

struct Emoji : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake, false, true> id; // null for unicode emojis
    Field<QString> name;
    Field<bool, true> animated;

    static Emoji fromJson(const QJsonObject &obj)
    {
        Emoji emoji;
        get(obj, "id", emoji.id);
        get(obj, "name", emoji.name);
        get(obj, "animated", emoji.animated);
        return emoji;
    }

    bool isUnicode() const { return !id.hasValue() || id.isNull(); }

    QString getImageUrl(int size = 48) const
    {
        if (isUnicode())
            return {};
        return QString("https://cdn.discordapp.com/emojis/%1.webp?size=%2")
                .arg(id->toString())
                .arg(size);
    }
};

struct ReactionCountDetails : Core::JsonUtils::JsonObject
{
    Field<int> burst;
    Field<int> normal;

    static ReactionCountDetails fromJson(const QJsonObject &obj)
    {
        ReactionCountDetails details;
        get(obj, "burst", details.burst);
        get(obj, "normal", details.normal);
        return details;
    }
};

struct Reaction : Core::JsonUtils::JsonObject
{
    Field<Emoji> emoji;
    Field<int> count;
    Field<ReactionCountDetails, true> countDetails;
    Field<bool> me;
    Field<bool, true> meBurst;
    Field<int, true> burstCount;
    Field<QList<QString>, true> burstColors;

    static Reaction fromJson(const QJsonObject &obj)
    {
        Reaction reaction;
        get(obj, "emoji", reaction.emoji);
        get(obj, "count", reaction.count);
        get(obj, "count_details", reaction.countDetails);
        get(obj, "me", reaction.me);
        get(obj, "me_burst", reaction.meBurst);
        get(obj, "burst_count", reaction.burstCount);
        get(obj, "burst_colors", reaction.burstColors);
        return reaction;
    }

    QColor getBrightestBurstColor() const
    {
        if (!burstColors.hasValue() || burstColors->isEmpty())
            return {};

        QColor best;
        float maxSaturation = -1;

        for (const QString &hex : *burstColors) {
            QColor color(hex);
            if (!color.isValid())
                continue;
            if (color.saturationF() > maxSaturation) {
                maxSaturation = color.saturationF();
                best = color;
            }
        }

        return best;
    }
};

struct MessageReference : Core::JsonUtils::JsonObject
{
    Field<int, true> type;
    Field<Core::Snowflake, true> messageId;
    Field<Core::Snowflake, true> channelId;
    Field<Core::Snowflake, true> guildId;

    static MessageReference fromJson(const QJsonObject &obj)
    {
        MessageReference ref;
        get(obj, "type", ref.type);
        get(obj, "message_id", ref.messageId);
        get(obj, "channel_id", ref.channelId);
        get(obj, "guild_id", ref.guildId);
        return ref;
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
    Field<QList<User>, true> mentions;
    Field<QList<Core::Snowflake>, true> mentionRoles;
    Field<QList<Reaction>, true> reactions;

    Field<MessageReference, true> messageReference;

    // tri-state for referenced_message:
    //   nullptr + referencedMessageNull=false  -> backend didn't fetch (unknown)
    //   nullptr + referencedMessageNull=true   -> referenced message was deleted
    //   non-null                               -> referenced message is present
    std::shared_ptr<Message> referencedMessage;
    bool referencedMessageNull = false;

    // TRANSIENT for MESSAGE_UPDATE
    Field<Core::Snowflake, true> guildId;
    Field<ChannelType, true> channelType;

    // cached data
    QString parsedContentCached;
    QString embedsJson;
    QString reactionsJson;

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
        get(obj, "mentions", message.mentions);
        get(obj, "mention_roles", message.mentionRoles);
        get(obj, "reactions", message.reactions);
        get(obj, "message_reference", message.messageReference);
        get(obj, "guild_id", message.guildId);
        get(obj, "channel_type", message.channelType);

        // referenced_message: manually handle tri-state (absent / null / object)
        auto refIt = obj.find("referenced_message");
        if (refIt != obj.end()) {
            if (refIt.value().isNull()) {
                message.referencedMessageNull = true;
            } else {
                message.referencedMessage =
                        std::make_shared<Message>(fromJson(refIt.value().toObject()));
            }
        }

        if (obj.contains("embeds")) {
            QJsonDocument doc(obj.value("embeds").toArray());
            message.embedsJson = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
        }

        if (obj.contains("reactions")) {
            QJsonDocument doc(obj.value("reactions").toArray());
            message.reactionsJson = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
        }

        return message;
    }
};

struct MuteConfig : Core::JsonUtils::JsonObject
{
    Field<QString, true, true> endTime;
    Field<int, true> selectedTimeWindow;

    static MuteConfig fromJson(const QJsonObject &obj)
    {
        MuteConfig config;
        get(obj, "end_time", config.endTime);
        get(obj, "selected_time_window", config.selectedTimeWindow);
        return config;
    }
};

struct ChannelOverride : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<bool, true> collapsed;
    Field<int, true> flags;
    Field<int, true> messageNotifications;
    Field<bool, true> muted;
    Field<MuteConfig, true, true> muteConfig;

    static ChannelOverride fromJson(const QJsonObject &obj)
    {
        ChannelOverride co;
        get(obj, "channel_id", co.channelId);
        get(obj, "collapsed", co.collapsed);
        get(obj, "flags", co.flags);
        get(obj, "message_notifications", co.messageNotifications);
        get(obj, "muted", co.muted);
        get(obj, "mute_config", co.muteConfig);
        return co;
    }
};

struct UserGuildSettings : Core::JsonUtils::JsonObject
{
    Field<QList<ChannelOverride>, true> channelOverrides;
    Field<int, true> flags;
    Field<Core::Snowflake, false, true> guildId;
    Field<bool, true> hideMutedChannels;
    Field<int, true> messageNotifications;
    Field<bool, true> mobilePush;
    Field<bool, true> muteScheduledEvents;
    Field<bool, true> muted;
    Field<MuteConfig, true, true> muteConfig;
    Field<int, true> notifyHighlights;
    Field<bool, true> suppressEveryone;
    Field<bool, true> suppressRoles;
    Field<int, true> version;

    static UserGuildSettings fromJson(const QJsonObject &obj)
    {
        UserGuildSettings s;
        get(obj, "channel_overrides", s.channelOverrides);
        get(obj, "flags", s.flags);
        get(obj, "guild_id", s.guildId);
        get(obj, "hide_muted_channels", s.hideMutedChannels);
        get(obj, "message_notifications", s.messageNotifications);
        get(obj, "mobile_push", s.mobilePush);
        get(obj, "mute_scheduled_events", s.muteScheduledEvents);
        get(obj, "muted", s.muted);
        get(obj, "mute_config", s.muteConfig);
        get(obj, "notify_highlights", s.notifyHighlights);
        get(obj, "suppress_everyone", s.suppressEveryone);
        get(obj, "suppress_roles", s.suppressRoles);
        get(obj, "version", s.version);
        return s;
    }
};

struct ReadStateEntry : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<int, true> readStateType;
    Field<Core::Snowflake, true, true> lastMessageId;
    Field<int, true> mentionCount;
    Field<int, true> flags;
    Field<int, true, true> lastViewed;
    Field<QString, true, true> lastPinTimestamp;

    static ReadStateEntry fromJson(const QJsonObject &obj)
    {
        ReadStateEntry entry;
        get(obj, "id", entry.id);
        get(obj, "read_state_type", entry.readStateType);
        get(obj, "last_message_id", entry.lastMessageId);
        get(obj, "mention_count", entry.mentionCount);
        get(obj, "flags", entry.flags);
        get(obj, "last_viewed", entry.lastViewed);
        get(obj, "last_pin_timestamp", entry.lastPinTimestamp);
        return entry;
    }
};

} // namespace Discord
} // namespace Acheron
