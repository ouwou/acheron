#pragma once

#include <QObject>
#include <QHash>
#include <QSet>
#include <QDateTime>
#include <QTimer>

#include <optional>

#include "Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"

namespace Acheron {
namespace Core {

class PermissionManager;

struct ChannelReadState
{
    bool isUnread = false;
    int mentionCount = 0;
    bool isMuted = false;
    bool countsForGuildUnread = false;
};

class ReadStateManager : public QObject
{
    Q_OBJECT
public:
    explicit ReadStateManager(Snowflake accountId, PermissionManager *perms,
                              QObject *parent = nullptr);

    void loadFromReady(const QList<Discord::ReadStateEntry> &readStates,
                       const QList<Discord::UserGuildSettings> &guildSettings);

    void setGuildReadInfo(Snowflake guildId, const QDateTime &joinedAt, Discord::MessageNotificationLevel defaultMessageNotifications);
    void registerChannelGuild(Snowflake channelId, Snowflake guildId);
    void removeGuild(Snowflake guildId);

    [[nodiscard]] ChannelReadState computeChannelReadState(Snowflake channelId, Snowflake guildId,
                                                           Snowflake parentId,
                                                           bool isDM = false) const;
    [[nodiscard]] ChannelReadState computeThreadReadState(Snowflake threadId, Snowflake guildId,
                                                          Snowflake parentId, bool joined) const;

    bool isChannelUnread(Snowflake channelId, Snowflake channelLastMessageId, Snowflake guildId) const;
    int getMentionCount(Snowflake channelId) const;
    bool isChannelMuted(Snowflake channelId) const;
    bool isGuildMuted(Snowflake guildId) const;

    [[nodiscard]] bool isForumPostUnread(Snowflake threadId, Snowflake lastMessageId, bool archived) const;
    // should it be in channel list
    [[nodiscard]] bool isThreadRelevant(const Discord::Channel &thread) const;
    [[nodiscard]] bool isForumPostNew(Snowflake threadId, Snowflake forumId, Snowflake guildId, bool archived) const;
    void markForumPostAsRead(Snowflake threadId, Snowflake lastMessageId);
    [[nodiscard]] bool hasBeenRead(Snowflake channelId) const;
    [[nodiscard]] Snowflake effectiveAckId(Snowflake channelId, Snowflake guildId) const;

    void onMessageAck(const Discord::MessageAck &ack);
    void onUserGuildSettingsUpdate(const Discord::UserGuildSettings &settings);
    void updateLocalReadState(Snowflake channelId, Snowflake lastMessageId);

    void setActiveChannel(Snowflake channelId);
    void markChannelAsRead(Snowflake channelId, Snowflake lastMessageId);
    void markChannelsAsRead(const QList<QPair<Snowflake, Snowflake>> &channelMessagePairs);
    void handleMessageCreated(Snowflake channelId, Snowflake messageId, bool isMention);

    void updateChannelLastMessageId(Snowflake channelId, Snowflake messageId);
    [[nodiscard]] Snowflake getChannelLastMessageId(Snowflake channelId) const;

    const Discord::UserGuildSettings *getGuildSettings(Snowflake guildId) const;
    std::optional<Discord::ReadStateEntry> getReadStateEntry(Snowflake channelId) const;

    static bool isMuteActive(bool muted, const Discord::MuteConfig *muteConfig);
    static int daysSinceDiscordEpoch();

signals:
    void readStateUpdated(Snowflake channelId);
    void guildSettingsUpdated(Snowflake guildId);
    void ackRequested(Snowflake channelId, Snowflake messageId);
    void bulkAckRequested(const QList<QPair<Snowflake, Snowflake>> &channelMessagePairs);

private:
    void rebuildChannelOverrideCache(Snowflake guildSettingsKey);

    Discord::MessageNotificationLevel resolveMessageNotifications(Snowflake guildId,
                                                                  Snowflake channelId,
                                                                  Snowflake parentId) const;
    Snowflake guildForChannel(Snowflake channelId) const;

    struct GuildReadInfo
    {
        qint64 joinedAtMs = 0;
        Discord::MessageNotificationLevel defaultMessageNotifications = Discord::MessageNotificationLevel::ALL_MESSAGES;
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

    Snowflake activeChannelId;
    QTimer activeChannelAckTimer;
    bool activeChannelAckPending = false;
};

} // namespace Core
} // namespace Acheron
