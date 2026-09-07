#pragma once

#include <QObject>
#include <QHash>
#include <QSet>
#include <QDateTime>

#include "ChannelReadState.hpp"
#include "Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"

namespace Acheron {
namespace Core {

class PermissionManager;

class ReadStateManager : public QObject
{
    Q_OBJECT
public:
    explicit ReadStateManager(Snowflake accountId, PermissionManager *perms,
                              QObject *parent = nullptr);

    void loadFromReady(const QList<Discord::ReadStateEntry> &readStates,
                       const QList<Discord::UserGuildSettings> &guildSettings,
                       int notificationSettingsFlags);
    void setNotificationSettingsFlags(int flags);

    void setGuildReadInfo(Snowflake guildId, const QDateTime &joinedAt,
                          Discord::MessageNotificationLevel defaultMessageNotifications,
                          bool isCommunity);
    void registerChannelGuild(Snowflake channelId, Snowflake guildId);
    void registerChannel(const Discord::Channel &channel, Snowflake guildId);
    void removeGuild(Snowflake guildId);

    [[nodiscard]] ChannelReadState computeChannelReadState(Snowflake channelId, Snowflake guildId, Snowflake parentId) const;
    [[nodiscard]] ChannelReadState computeDMReadState(Snowflake channelId) const;
    [[nodiscard]] ChannelReadState computeThreadReadState(Snowflake threadId, Snowflake guildId,
                                                          Snowflake parentId, Snowflake categoryId,
                                                          bool joined) const;
    [[nodiscard]] ChannelReadState computeForumPostReadState(Snowflake postId, Snowflake guildId,
                                                             Snowflake forumId, Snowflake categoryId) const;

    bool isChannelUnread(Snowflake channelId, Snowflake channelLastMessageId, Snowflake guildId) const;
    [[nodiscard]] bool hasUnreadOrMentions(Snowflake channelId) const;
    int getMentionCount(Snowflake channelId) const;
    bool isChannelMuted(Snowflake channelId) const;
    bool isGuildMuted(Snowflake guildId) const;
    [[nodiscard]] bool isSuppressEveryone(Snowflake guildId) const;
    [[nodiscard]] bool isSuppressRoles(Snowflake guildId) const;

    [[nodiscard]] bool isForumPostUnread(Snowflake threadId, Snowflake lastMessageId, bool archived) const;
    // should it be in channel list
    [[nodiscard]] bool isThreadRelevant(const Discord::Channel &thread) const;
    [[nodiscard]] bool isForumPostNew(Snowflake threadId, Snowflake forumId, Snowflake guildId, bool archived) const;
    void markForumPostAsRead(Snowflake threadId, Snowflake lastMessageId);
    [[nodiscard]] bool hasBeenRead(Snowflake channelId) const;
    [[nodiscard]] Snowflake effectiveAckId(Snowflake channelId, Snowflake guildId) const;

    void onMessageAck(const Discord::MessageAck &ack);
    void onUserGuildSettingsUpdate(const Discord::UserGuildSettings &settings);
    void onNotificationSettingsUpdate(const Discord::NotificationSettings &settings);

    void setActiveChannel(Snowflake channelId);
    void setActiveChannelAtBottom(bool atBottom);
    void markChannelAsRead(Snowflake channelId, Snowflake lastMessageId);
    void markChannelsAsRead(const QList<QPair<Snowflake, Snowflake>> &channelMessagePairs);
    void handleMessageCreated(Snowflake channelId, Snowflake messageId, bool fromSelf, bool isMention);

    void updateChannelLastMessageId(Snowflake channelId, Snowflake messageId);
    [[nodiscard]] Snowflake getChannelLastMessageId(Snowflake channelId) const;

    static bool isMuteActive(bool muted, const Discord::MuteConfig *muteConfig);
    static int daysSinceDiscordEpoch();

signals:
    void readStateUpdated(Snowflake channelId);
    void guildSettingsUpdated(Snowflake guildId);
    void ackRequested(Snowflake channelId, Snowflake messageId);
    void bulkAckRequested(const QList<QPair<Snowflake, Snowflake>> &channelMessagePairs);

private:
    Discord::ReadStateEntry &entryFor(Snowflake channelId);
    [[nodiscard]] bool hasUnreadOrMentions(Snowflake channelId, Snowflake lastMessageId) const;
    void ackLocally(Snowflake channelId, Snowflake messageId);
    void ack(Snowflake channelId, Snowflake messageId, bool immediate);
    void flushOutgoingAck(Snowflake channelId);
    [[nodiscard]] bool canAutoAckActiveChannel() const;
    void tryAckActiveChannel();

    void rebuildChannelOverrideCache(Snowflake guildSettingsKey);

    Discord::MessageNotificationLevel resolveMessageNotifications(Snowflake guildId,
                                                                  Snowflake channelId,
                                                                  Snowflake parentId) const;
    [[nodiscard]] bool unreadCountsForGuild(Snowflake guildId, Snowflake channelId, Snowflake parentId) const;
    [[nodiscard]] bool isMutedThroughParents(Snowflake channelId, Snowflake parentId, Snowflake categoryId, Snowflake guildId) const;
    [[nodiscard]] bool canTrackUnreads(Snowflake channelId) const;
    [[nodiscard]] bool isOptInGuild(Snowflake guildId) const;
    [[nodiscard]] bool isChannelOptedIn(Snowflake channelId) const;
    [[nodiscard]] bool hasRecentlyVisitedAndRead(Snowflake channelId, Snowflake lastMessageId) const;
    Snowflake guildForChannel(Snowflake channelId) const;

    struct GuildReadInfo
    {
        qint64 joinedAtMs = 0;
        Discord::MessageNotificationLevel defaultMessageNotifications = Discord::MessageNotificationLevel::ALL_MESSAGES;
        bool isCommunity = false;
    };

    Snowflake accountId;
    PermissionManager *permissionManager;

    QHash<Snowflake, Discord::ReadStateEntry> channelReadStates;
    QHash<Snowflake, Snowflake> channelLastMessageIds;
    QHash<Snowflake, Snowflake> ackIdAtSelect;
    QHash<Snowflake, Discord::UserGuildSettings> guildSettingsMap; // Snowflake(0) for DMs

    QHash<Snowflake, Discord::ChannelOverride> channelOverrideCache;
    QHash<Snowflake, QSet<Snowflake>> guildOverrideChannels;

    QHash<Snowflake, GuildReadInfo> guildInfo;
    QHash<Snowflake, Snowflake> channelGuildMap;
    QSet<Snowflake> resourceChannels;
    QSet<Snowflake> voiceChannels;
    bool useNewNotifications = false;

    Snowflake activeChannelId;
    bool activeChannelAtBottom = true;
    QHash<Snowflake, Snowflake> outgoingAcks;
};

} // namespace Core
} // namespace Acheron
