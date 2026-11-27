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
    void updateGuildDefaults(Snowflake guildId, Discord::MessageNotificationLevel defaultMessageNotifications, bool isCommunity);
    void registerChannelGuild(Snowflake channelId, Snowflake guildId);
    void registerChannel(const Discord::Channel &channel, Snowflake guildId);
    // atReady: Discord sends no read state for a DM nothing new has come into since it was last
    // read (often an old one), and the official client counts those as read, so Acheron does too
    void registerPrivateChannel(const Discord::Channel &channel, bool atReady = false);
    void registerThreadParent(Snowflake threadId, Snowflake parentId);
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

    // atBottom false when it opens scrolled up (at its unreads): nothing is marked read until it reaches the bottom
    void setActiveChannel(Snowflake channelId, bool atBottom = true);
    void setActiveChannelAtBottom(bool atBottom);
    void markChannelAsRead(Snowflake channelId, Snowflake lastMessageId);
    void markChannelsAsRead(const QList<Snowflake> &channelIds);
    void handleMessageCreated(Snowflake channelId, Snowflake messageId, bool fromSelf, bool isMention);

    void updateChannelLastMessageId(Snowflake channelId, Snowflake messageId);
    [[nodiscard]] Snowflake getChannelLastMessageId(Snowflake channelId) const;

    void registerChannelLastPin(const Discord::Channel &channel);
    [[nodiscard]] bool hasUnreadPins(Snowflake channelId, Snowflake guildId, Snowflake parentId, bool isThread, bool joinedThread) const;
    void ackPins(Snowflake channelId);
    void onChannelPinsUpdate(const Discord::ChannelPinsUpdate &event);
    void onChannelPinsAck(const Discord::ChannelPinsAck &event);

    static bool isMuteActive(bool muted, const Discord::MuteConfig *muteConfig);
    static int daysSinceDiscordEpoch();

signals:
    void readStateUpdated(Snowflake channelId);
    void guildSettingsUpdated(Snowflake guildId);
    void ackRequested(Snowflake channelId, Snowflake messageId);
    void bulkAckRequested(const QList<QPair<Snowflake, Snowflake>> &channelMessagePairs);
    void pinsReadStateChanged(Snowflake channelId);
    void pinsAckRequested(Snowflake channelId);

private:
    Discord::ReadStateEntry &entryFor(Snowflake channelId);
    [[nodiscard]] Snowflake markAsReadMessageId(Snowflake channelId) const;
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
    [[nodiscard]] bool optInHidesUnread(Snowflake channelId, Snowflake parentId, Snowflake lastMessageId, int mentionCount) const;
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
    QHash<Snowflake, qint64> channelLastPinMs;
    QHash<Snowflake, qint64> channelAckPinMs;
    QHash<Snowflake, Snowflake> ackIdAtSelect;
    QHash<Snowflake, Discord::UserGuildSettings> guildSettingsMap; // Snowflake(0) for DMs

    QHash<Snowflake, Discord::ChannelOverride> channelOverrideCache;
    QHash<Snowflake, QSet<Snowflake>> guildOverrideChannels;

    QHash<Snowflake, GuildReadInfo> guildInfo;
    QHash<Snowflake, Snowflake> channelGuildMap;
    QHash<Snowflake, Snowflake> threadParents;
    QSet<Snowflake> resourceChannels;
    QSet<Snowflake> voiceChannels;
    QSet<Snowflake> forumChannels;
    QSet<Snowflake> messageRequestChannels;
    bool useNewNotifications = false;

    Snowflake activeChannelId;
    bool activeChannelAtBottom = true;
    QHash<Snowflake, Snowflake> outgoingAcks;
};

} // namespace Core
} // namespace Acheron
