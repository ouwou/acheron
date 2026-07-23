#pragma once

#include <QHash>
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
    Field<QList<ReadStateEntry>, true> readState;
    Field<QList<UserGuildSettings>, true> userGuildSettings;
    Field<UserSettings, true> userSettings;
    Field<QList<Relationship>> relationships;
    Field<QString> sessionId;
    Field<QString> resumeGatewayUrl;
    Field<QHash<Core::Snowflake, QString>> notes;

    static Ready fromJson(const QJsonObject &obj)
    {
        Ready ready;
        get(obj, "user", ready.user);
        get(obj, "guilds", ready.guilds);
        get(obj, "user_settings_proto", ready.userSettingsProto);
        get(obj, "user_settings", ready.userSettings);
        get(obj, "merged_members", ready.mergedMembers);
        get(obj, "users", ready.users);
        get(obj, "private_channels", ready.privateChannels);
        get(obj, "relationships", ready.relationships);
        get(obj, "session_id", ready.sessionId);
        get(obj, "resume_gateway_url", ready.resumeGatewayUrl);
        get(obj, "notes", ready.notes);

        if (obj.contains("read_state")) {
            QJsonObject rsObj = obj["read_state"].toObject();
            if (rsObj.contains("entries")) {
                QJsonArray arr = rsObj["entries"].toArray();
                QList<ReadStateEntry> entries;
                entries.reserve(arr.size());
                for (const QJsonValue &val : arr)
                    entries.append(ReadStateEntry::fromJson(val.toObject()));
                ready.readState = entries;
            }
        }

        if (obj.contains("user_guild_settings")) {
            QJsonObject ugsObj = obj["user_guild_settings"].toObject();
            if (ugsObj.contains("entries")) {
                QJsonArray arr = ugsObj["entries"].toArray();
                QList<UserGuildSettings> entries;
                entries.reserve(arr.size());
                for (const QJsonValue &val : arr)
                    entries.append(UserGuildSettings::fromJson(val.toObject()));
                ready.userGuildSettings = entries;
            }
        }

        return ready;
    }
};

struct SupplementalGuild : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<QList<VoiceState>, true> voiceStates;

    static SupplementalGuild fromJson(const QJsonObject &obj)
    {
        SupplementalGuild supplementalGuild;
        get(obj, "id", supplementalGuild.id);
        get(obj, "voice_states", supplementalGuild.voiceStates);
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
        typingStart.timestamp = QDateTime::fromSecsSinceEpoch(obj["timestamp"].toVariant().toLongLong());
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

struct ThreadDelete : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<Core::Snowflake, true, true> parentId;
    Field<Core::Snowflake, true> guildId;

    static ThreadDelete fromJson(const QJsonObject &obj)
    {
        ThreadDelete event;
        get(obj, "id", event.id);
        get(obj, "parent_id", event.parentId);
        get(obj, "guild_id", event.guildId);
        return event;
    }
};

struct ForumUnread : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> threadId;
    Field<int, true> count;

    static ForumUnread fromJson(const QJsonObject &obj)
    {
        ForumUnread unread;
        get(obj, "thread_id", unread.threadId);
        get(obj, "count", unread.count);
        return unread;
    }
};

struct ForumUnreads : Core::JsonUtils::JsonObject
{
    Field<QList<ForumUnread>, true> threads;
    Field<bool, true> permissionDenied;

    static ForumUnreads fromJson(const QJsonObject &obj)
    {
        ForumUnreads event;
        get(obj, "threads", event.threads);
        get(obj, "permission_denied", event.permissionDenied);
        return event;
    }
};

struct ThreadListSync : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<QList<Core::Snowflake>, true> channelIds;
    Field<QList<Channel>, true> threads;
    Field<QList<ThreadMember>, true> members;

    static ThreadListSync fromJson(const QJsonObject &obj)
    {
        ThreadListSync event;
        get(obj, "guild_id", event.guildId);
        get(obj, "channel_ids", event.channelIds);
        get(obj, "threads", event.threads);
        get(obj, "members", event.members);
        return event;
    }
};

struct ThreadMemberUpdate : Core::JsonUtils::JsonObject
{
    ThreadMember member;
    Field<Core::Snowflake, true> userId;
    Field<Core::Snowflake, true> guildId;

    static ThreadMemberUpdate fromJson(const QJsonObject &obj)
    {
        ThreadMemberUpdate event;
        event.member = ThreadMember::fromJson(obj);
        get(obj, "user_id", event.userId);
        get(obj, "guild_id", event.guildId);
        return event;
    }
};

struct ThreadMembersUpdate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id; // thread id
    Field<Core::Snowflake, true> guildId;
    Field<int, true> memberCount;
    Field<QList<ThreadMember>, true> addedMembers;
    Field<QList<Core::Snowflake>, true> removedMemberIds;

    static ThreadMembersUpdate fromJson(const QJsonObject &obj)
    {
        ThreadMembersUpdate event;
        get(obj, "id", event.id);
        get(obj, "guild_id", event.guildId);
        get(obj, "member_count", event.memberCount);
        get(obj, "added_members", event.addedMembers);
        get(obj, "removed_member_ids", event.removedMemberIds);
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

struct GuildMemberUpdate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<Member> member;

    static GuildMemberUpdate fromJson(const QJsonObject &obj)
    {
        GuildMemberUpdate event;
        get(obj, "guild_id", event.guildId);
        event.member = Member::fromJson(obj);
        return event;
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

struct GuildRoleCreate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<Role> role;

    static GuildRoleCreate fromJson(const QJsonObject &obj)
    {
        GuildRoleCreate event;
        get(obj, "guild_id", event.guildId);
        get(obj, "role", event.role);
        return event;
    }
};

struct GuildRoleUpdate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<Role> role;

    static GuildRoleUpdate fromJson(const QJsonObject &obj)
    {
        GuildRoleUpdate event;
        get(obj, "guild_id", event.guildId);
        get(obj, "role", event.role);
        return event;
    }
};

struct GuildRoleDelete : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<Core::Snowflake> roleId;

    static GuildRoleDelete fromJson(const QJsonObject &obj)
    {
        GuildRoleDelete event;
        get(obj, "guild_id", event.guildId);
        get(obj, "role_id", event.roleId);
        return event;
    }
};

struct MessageAck : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<int, true> mentionCount;
    Field<int, true> flags;
    Field<int, true> version;

    static MessageAck fromJson(const QJsonObject &obj)
    {
        MessageAck ack;
        get(obj, "channel_id", ack.channelId);
        get(obj, "message_id", ack.messageId);
        get(obj, "mention_count", ack.mentionCount);
        get(obj, "flags", ack.flags);
        get(obj, "version", ack.version);
        return ack;
    }
};

struct MessageReactionAdd : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> userId;
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<Core::Snowflake, true> messageAuthorId;
    Field<Core::Snowflake, true> guildId;
    Field<Emoji> emoji;
    Field<int, true> type; // 0 = normal, 1 = burst
    Field<QList<QString>, true> burstColors;

    static MessageReactionAdd fromJson(const QJsonObject &obj)
    {
        MessageReactionAdd event;
        get(obj, "user_id", event.userId);
        get(obj, "channel_id", event.channelId);
        get(obj, "message_id", event.messageId);
        get(obj, "message_author_id", event.messageAuthorId);
        get(obj, "guild_id", event.guildId);
        get(obj, "emoji", event.emoji);
        get(obj, "type", event.type);
        get(obj, "burst_colors", event.burstColors);
        return event;
    }
};

struct DebouncedReaction : Core::JsonUtils::JsonObject
{
    Field<QList<Core::Snowflake>> users;
    Field<Emoji> emoji;

    static DebouncedReaction fromJson(const QJsonObject &obj)
    {
        DebouncedReaction reaction;
        get(obj, "users", reaction.users);
        get(obj, "emoji", reaction.emoji);
        return reaction;
    }
};

struct MessageReactionAddMany : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<Core::Snowflake, true> guildId;
    Field<QList<DebouncedReaction>> reactions;

    static MessageReactionAddMany fromJson(const QJsonObject &obj)
    {
        MessageReactionAddMany event;
        get(obj, "channel_id", event.channelId);
        get(obj, "message_id", event.messageId);
        get(obj, "guild_id", event.guildId);
        get(obj, "reactions", event.reactions);
        return event;
    }
};

struct MessageReactionRemove : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> userId;
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<Core::Snowflake, true> guildId;
    Field<Emoji> emoji;
    Field<int, true> type;

    static MessageReactionRemove fromJson(const QJsonObject &obj)
    {
        MessageReactionRemove event;
        get(obj, "user_id", event.userId);
        get(obj, "channel_id", event.channelId);
        get(obj, "message_id", event.messageId);
        get(obj, "guild_id", event.guildId);
        get(obj, "emoji", event.emoji);
        get(obj, "type", event.type);
        return event;
    }
};

struct MessageReactionRemoveAll : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<Core::Snowflake, true> guildId;

    static MessageReactionRemoveAll fromJson(const QJsonObject &obj)
    {
        MessageReactionRemoveAll event;
        get(obj, "channel_id", event.channelId);
        get(obj, "message_id", event.messageId);
        get(obj, "guild_id", event.guildId);
        return event;
    }
};

struct MessageReactionRemoveEmoji : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<Core::Snowflake, true> guildId;
    Field<Emoji> emoji;

    static MessageReactionRemoveEmoji fromJson(const QJsonObject &obj)
    {
        MessageReactionRemoveEmoji event;
        get(obj, "channel_id", event.channelId);
        get(obj, "message_id", event.messageId);
        get(obj, "guild_id", event.guildId);
        get(obj, "emoji", event.emoji);
        return event;
    }
};

struct GuildMemberListUpdate : Core::JsonUtils::JsonObject
{
    struct Group : Core::JsonUtils::JsonObject
    {
        Field<QString> id; // role snowflake string, "online", or "offline"
        Field<int> count;

        static Group fromJson(const QJsonObject &obj)
        {
            Group group;
            get(obj, "id", group.id);
            get(obj, "count", group.count);
            return group;
        }
    };

    struct SyncItem : Core::JsonUtils::JsonObject
    {
        Field<Group, true> group;
        Field<Member, true> member;

        static SyncItem fromJson(const QJsonObject &obj)
        {
            SyncItem item;
            if (obj.contains("group"))
                item.group = Group::fromJson(obj["group"].toObject());
            if (obj.contains("member"))
                item.member = Member::fromJson(obj["member"].toObject());
            return item;
        }
    };

    struct ListOp : Core::JsonUtils::JsonObject
    {
        Field<QString> op;
        Field<QPair<int, int>, true> range;
        Field<QList<SyncItem>, true> items;
        Field<int, true> index;
        Field<SyncItem, true> item;

        static ListOp fromJson(const QJsonObject &obj)
        {
            ListOp listOp;
            get(obj, "op", listOp.op);
            get(obj, "index", listOp.index);

            if (obj.contains("range")) {
                QJsonArray rangeArr = obj["range"].toArray();
                if (rangeArr.size() == 2)
                    listOp.range = QPair<int, int>(rangeArr[0].toInt(), rangeArr[1].toInt());
            }

            if (obj.contains("items")) {
                QJsonArray itemsArr = obj["items"].toArray();
                QList<SyncItem> syncItems;
                syncItems.reserve(itemsArr.size());
                for (const QJsonValue &val : itemsArr)
                    syncItems.append(SyncItem::fromJson(val.toObject()));
                listOp.items = syncItems;
            }

            if (obj.contains("item"))
                listOp.item = SyncItem::fromJson(obj["item"].toObject());

            return listOp;
        }
    };

    Field<QString> id; // member list id
    Field<Core::Snowflake> guildId;
    Field<QList<Group>> groups;
    Field<QList<ListOp>> ops;
    Field<int> memberCount;
    Field<int> onlineCount;

    static GuildMemberListUpdate fromJson(const QJsonObject &obj)
    {
        GuildMemberListUpdate event;
        get(obj, "id", event.id);
        get(obj, "guild_id", event.guildId);
        get(obj, "member_count", event.memberCount);
        get(obj, "online_count", event.onlineCount);

        if (obj.contains("groups")) {
            QJsonArray groupsArr = obj["groups"].toArray();
            QList<Group> groups;
            groups.reserve(groupsArr.size());
            for (const QJsonValue &val : groupsArr)
                groups.append(Group::fromJson(val.toObject()));
            event.groups = groups;
        }

        if (obj.contains("ops")) {
            QJsonArray opsArr = obj["ops"].toArray();
            QList<ListOp> ops;
            ops.reserve(opsArr.size());
            for (const QJsonValue &val : opsArr)
                ops.append(ListOp::fromJson(val.toObject()));
            event.ops = ops;
        }

        return event;
    }
};

struct VoiceServerUpdate : Core::JsonUtils::JsonObject
{
    Field<QString> token;
    Field<Core::Snowflake> guildId;
    Field<QString, false, true> endpoint;

    static VoiceServerUpdate fromJson(const QJsonObject &obj)
    {
        VoiceServerUpdate event;
        get(obj, "token", event.token);
        get(obj, "guild_id", event.guildId);
        get(obj, "endpoint", event.endpoint);
        return event;
    }
};

struct VoiceStateUpdateBatch : Core::JsonUtils::JsonObject
{
    Field<QList<VoiceState>, true> voiceStates;

    static VoiceStateUpdateBatch fromJson(const QJsonObject &obj)
    {
        VoiceStateUpdateBatch event;
        get(obj, "voice_states", event.voiceStates);
        return event;
    }
};

struct RelationshipPartial : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<RelationshipType> type;
    Field<QString, false, true> nickname;
    Field<QDateTime, true> since;
    Field<bool, true> strangerRequest;
    Field<bool> userIgnored;

    static RelationshipPartial fromJson(const QJsonObject &obj)
    {
        RelationshipPartial r;
        get(obj, "id", r.id);
        get(obj, "type", r.type);
        get(obj, "nickname", r.nickname);
        get(obj, "since", r.since);
        get(obj, "stranger_request", r.strangerRequest);
        get(obj, "user_ignored", r.userIgnored);
        return r;
    }
};

struct UserNoteUpdate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<QString> note;

    static UserNoteUpdate fromJson(const QJsonObject &obj)
    {
        UserNoteUpdate n;
        get(obj, "id", n.id);
        get(obj, "note", n.note);
        return n;
    }
};

} // namespace Discord
} // namespace Acheron
