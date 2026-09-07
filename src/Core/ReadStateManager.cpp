#include "ReadStateManager.hpp"

#include <QGuiApplication>
#include <QTimeZone>
#include <QTimer>

#include <optional>

#include "Core/Logging.hpp"
#include "Core/PermissionManager.hpp"
#include "Discord/Enums.hpp"

namespace Acheron {
namespace Core {

namespace {

constexpr int kDeferredAckDelayMs = 3000;
constexpr qint64 kMsPerDay = 24 * 60 * 60 * 1000;

bool isOlderThanDays(Snowflake id, int days)
{
    if (!id.isValid())
        return true;
    return id.toDateTime().toMSecsSinceEpoch() < QDateTime::currentMSecsSinceEpoch() - days * kMsPerDay;
}

template <typename Settings, typename Flag>
bool hasFlag(const Settings &settings, Flag flag)
{
    return settings.flags.hasValue() && (settings.flags.get() & static_cast<int>(flag)) != 0;
}

bool usesNewNotifications(int notificationSettingsFlags)
{
    return (notificationSettingsFlags & static_cast<int>(Discord::NotificationSettingsFlag::USE_NEW_NOTIFICATIONS)) != 0;
}

template <typename Settings>
bool isMutedSetting(const Settings &settings)
{
    bool muted = settings.muted.hasValue() && settings.muted.get();
    const Discord::MuteConfig *config = settings.muteConfig.hasValue() ? &settings.muteConfig.get() : nullptr;
    return ReadStateManager::isMuteActive(muted, config);
}

} // namespace

ReadStateManager::ReadStateManager(Snowflake accountId, PermissionManager *perms, QObject *parent)
    : QObject(parent), accountId(accountId), permissionManager(perms)
{
    if (qGuiApp) {
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive)
                tryAckActiveChannel();
        });
    }
}

void ReadStateManager::loadFromReady(const QList<Discord::ReadStateEntry> &readStates,
                                     const QList<Discord::UserGuildSettings> &guildSettings,
                                     int notificationSettingsFlags)
{
    channelReadStates.clear();
    guildSettingsMap.clear();
    guildInfo.clear();
    channelGuildMap.clear();
    resourceChannels.clear();
    voiceChannels.clear();
    ackIdAtSelect.clear();
    outgoingAcks.clear();

    for (const auto &entry : readStates) {
        int rsType = entry.readStateType.hasValue() ? entry.readStateType.get() : 0;
        if (rsType != 0)
            continue;

        channelReadStates.insert(entry.id.get(), entry);
    }

    channelOverrideCache.clear();
    guildOverrideChannels.clear();

    for (const auto &settings : guildSettings) {
        Snowflake key = settings.guildId.isNull() ? Snowflake(0) : settings.guildId.get();
        guildSettingsMap.insert(key, settings);
        rebuildChannelOverrideCache(key);
    }

    useNewNotifications = usesNewNotifications(notificationSettingsFlags);

    qCDebug(LogCore) << "ReadStateManager loaded" << channelReadStates.size() << "read states and"
                     << guildSettingsMap.size() << "guild settings";
}

void ReadStateManager::setNotificationSettingsFlags(int flags)
{
    bool enabled = usesNewNotifications(flags);
    if (useNewNotifications == enabled)
        return;

    useNewNotifications = enabled;
    for (auto it = guildInfo.constBegin(); it != guildInfo.constEnd(); ++it)
        emit guildSettingsUpdated(it.key());
}

void ReadStateManager::onNotificationSettingsUpdate(const Discord::NotificationSettings &settings)
{
    if (settings.flags.hasValue())
        setNotificationSettingsFlags(settings.flags.get());
}

ChannelReadState ReadStateManager::computeChannelReadState(Snowflake channelId, Snowflake guildId,
                                                           Snowflake parentId) const
{
    ChannelReadState result;
    result.isMuted = isChannelMuted(channelId);
    if (!canTrackUnreads(channelId))
        return result;

    Snowflake lastMessageId = getChannelLastMessageId(channelId);
    bool canReadHistory = permissionManager->hasChannelPermission(accountId, channelId, Discord::Permission::READ_MESSAGE_HISTORY);
    result.mentionCount = canReadHistory ? getMentionCount(channelId) : 0;
    result.isUnread = isChannelUnread(channelId, lastMessageId, guildId);

    bool isVoice = voiceChannels.contains(channelId);
    if (isVoice && !permissionManager->hasChannelPermission(accountId, channelId, Discord::Permission::CONNECT))
        result.mentionCount = 0;

    if (isOptInGuild(guildId)) {
        bool optedIn = isChannelOptedIn(channelId) || (parentId.isValid() && isChannelOptedIn(parentId));
        if (isOlderThanDays(lastMessageId, 7)) {
            result.mentionCount = 0;
            result.isUnread = false;
        } else if (result.mentionCount == 0 && !optedIn && !hasRecentlyVisitedAndRead(channelId, lastMessageId)) {
            result.isUnread = false;
        }
    }

    bool countsWhenUnread = result.mentionCount > 0 || (!isVoice && unreadCountsForGuild(guildId, channelId, parentId));
    result.countsForGuildUnread = result.isUnread &&
                                  countsWhenUnread &&
                                  !isMutedThroughParents(channelId, parentId, Snowflake::Invalid, guildId);

    return result;
}

ChannelReadState ReadStateManager::computeDMReadState(Snowflake channelId) const
{
    ChannelReadState result;
    result.isMuted = isChannelMuted(channelId);
    result.mentionCount = getMentionCount(channelId);
    result.isUnread = isChannelUnread(channelId, getChannelLastMessageId(channelId), Snowflake::Invalid);
    result.countsForGuildUnread = result.isUnread && !result.isMuted;
    return result;
}

ChannelReadState ReadStateManager::computeThreadReadState(Snowflake threadId, Snowflake guildId,
                                                          Snowflake parentId, Snowflake categoryId,
                                                          bool joined) const
{
    ChannelReadState result;
    result.isMuted = isChannelMuted(threadId) ||
                     (parentId.isValid() && isChannelMuted(parentId)) ||
                     (categoryId.isValid() && isChannelMuted(categoryId));
    if (!canTrackUnreads(threadId))
        return result;

    result.mentionCount = getMentionCount(threadId);
    result.isUnread = joined ? isChannelUnread(threadId, getChannelLastMessageId(threadId), guildId)
                             : result.mentionCount > 0;
    result.countsForGuildUnread = result.isUnread &&
                                  !isMutedThroughParents(threadId, parentId, categoryId, guildId);
    return result;
}

ChannelReadState ReadStateManager::computeForumPostReadState(Snowflake postId, Snowflake guildId,
                                                             Snowflake forumId, Snowflake categoryId) const
{
    ChannelReadState result;
    result.isMuted = isChannelMuted(postId);
    if (!canTrackUnreads(postId))
        return result;

    result.mentionCount = getMentionCount(postId);
    result.isUnread = isForumPostUnread(postId, getChannelLastMessageId(postId), false);
    result.countsForGuildUnread = result.isUnread &&
                                  !isMutedThroughParents(postId, forumId, categoryId, guildId);
    return result;
}

// Server Guide resource channels never track unreads or mentions, whatever their read state says.
bool ReadStateManager::canTrackUnreads(Snowflake channelId) const
{
    if (resourceChannels.contains(channelId))
        return false;
    return permissionManager->hasChannelPermission(accountId, channelId, Discord::Permission::VIEW_CHANNEL);
}

bool ReadStateManager::isOptInGuild(Snowflake guildId) const
{
    auto gi = guildInfo.constFind(guildId);
    if (gi == guildInfo.constEnd() || !gi->isCommunity)
        return false;
    auto gs = guildSettingsMap.constFind(guildId);
    return gs != guildSettingsMap.constEnd() && hasFlag(gs.value(), Discord::UserGuildSettingsFlag::OPT_IN_CHANNELS_ON);
}

bool ReadStateManager::isChannelOptedIn(Snowflake channelId) const
{
    auto it = channelOverrideCache.constFind(channelId);
    return it != channelOverrideCache.constEnd() && hasFlag(it.value(), Discord::ChannelOverrideFlag::OPT_IN_ENABLED);
}

bool ReadStateManager::hasRecentlyVisitedAndRead(Snowflake channelId, Snowflake lastMessageId) const
{
    if (!lastMessageId.isValid())
        return false;
    auto it = channelReadStates.constFind(channelId);
    return it != channelReadStates.constEnd() &&
           it->lastMessageId.hasValue() &&
           !isOlderThanDays(it->lastMessageId.get(), 3);
}

bool ReadStateManager::isMutedThroughParents(Snowflake channelId, Snowflake parentId, Snowflake categoryId,
                                             Snowflake guildId) const
{
    return isChannelMuted(channelId) ||
           (parentId.isValid() && isChannelMuted(parentId)) ||
           (categoryId.isValid() && isChannelMuted(categoryId)) ||
           (guildId.isValid() && isGuildMuted(guildId));
}

bool ReadStateManager::isChannelUnread(Snowflake channelId, Snowflake channelLastMessageId, Snowflake guildId) const
{
    if (!channelLastMessageId.isValid())
        return false;

    return channelLastMessageId > effectiveAckId(channelId, guildId);
}

bool ReadStateManager::hasUnreadOrMentions(Snowflake channelId) const
{
    return hasUnreadOrMentions(channelId, getChannelLastMessageId(channelId));
}

bool ReadStateManager::hasUnreadOrMentions(Snowflake channelId, Snowflake lastMessageId) const
{
    return getMentionCount(channelId) > 0 ||
           isChannelUnread(channelId, lastMessageId, guildForChannel(channelId));
}

bool ReadStateManager::hasBeenRead(Snowflake channelId) const
{
    auto it = channelReadStates.constFind(channelId);
    return it != channelReadStates.constEnd() && it->lastMessageId.hasValue();
}

bool ReadStateManager::isForumPostUnread(Snowflake threadId, Snowflake lastMessageId,
                                         bool archived) const
{
    if (archived || !lastMessageId.isValid())
        return false;

    auto it = channelReadStates.constFind(threadId);
    if (it != channelReadStates.constEnd() && it->lastMessageId.hasValue())
        return lastMessageId > it->lastMessageId.get();

    return true;
}

bool ReadStateManager::isThreadRelevant(const Discord::Channel &thread) const
{
    const Snowflake threadId = thread.id.get();
    if (thread.isPinned())
        return true;
    if (getMentionCount(threadId) > 0)
        return true;
    if (isForumPostUnread(threadId, thread.effectiveLastMessageId(), thread.isArchived()) &&
        !isChannelMuted(threadId))
        return true;

    if (!thread.threadMetadata.hasValue() || !thread.threadMetadata->autoArchiveDuration.hasValue())
        return false;
    const auto &meta = thread.threadMetadata.get();
    qint64 base = thread.effectiveLastMessageId().toDateTime().toMSecsSinceEpoch();
    if (meta.archiveTimestamp.hasValue())
        base = qMax(base, meta.archiveTimestamp.get().toMSecsSinceEpoch());
    const qint64 expiry = base + static_cast<qint64>(meta.autoArchiveDuration.get()) * 60000;
    return expiry > QDateTime::currentMSecsSinceEpoch();
}

bool ReadStateManager::isForumPostNew(Snowflake threadId, Snowflake forumId, Snowflake guildId,
                                      bool archived) const
{
    if (archived || !threadId.isValid() || !forumId.isValid())
        return false;

    if (hasBeenRead(threadId))
        return false;

    auto snap = ackIdAtSelect.constFind(forumId);
    if (snap == ackIdAtSelect.constEnd() || threadId <= snap.value())
        return false;

    if (!guildId.isValid())
        return true;

    auto gi = guildInfo.constFind(guildId);
    qint64 joinedAtMs = gi != guildInfo.constEnd() && gi->joinedAtMs > 0
                                ? gi->joinedAtMs
                                : QDateTime::currentMSecsSinceEpoch();
    return threadId.toDateTime().toMSecsSinceEpoch() > joinedAtMs;
}

void ReadStateManager::markForumPostAsRead(Snowflake threadId, Snowflake lastMessageId)
{
    if (!lastMessageId.isValid())
        return;

    auto it = channelReadStates.constFind(threadId);
    if (it != channelReadStates.constEnd() && it->lastMessageId.hasValue() && lastMessageId <= it->lastMessageId.get())
        return;

    ack(threadId, lastMessageId, true);
}

Snowflake ReadStateManager::effectiveAckId(Snowflake channelId, Snowflake guildId) const
{
    auto it = channelReadStates.constFind(channelId);
    if (it != channelReadStates.constEnd() && it->lastMessageId.hasValue())
        return it->lastMessageId.get();

    if (guildId.isValid()) {
        auto gi = guildInfo.constFind(guildId);
        if (gi != guildInfo.constEnd() && gi->joinedAtMs > 0)
            return Snowflake::fromUnixMs(gi->joinedAtMs);
        return Snowflake::fromUnixMs(QDateTime::currentMSecsSinceEpoch());
    }

    if (channelId.isValid())
        return Snowflake::fromUnixMs(channelId.toDateTime().toMSecsSinceEpoch());

    return Snowflake::fromUnixMs(QDateTime::currentMSecsSinceEpoch());
}

Discord::MessageNotificationLevel ReadStateManager::resolveMessageNotifications(Snowflake guildId,
                                                                                Snowflake channelId,
                                                                                Snowflake parentId) const
{
    using Level = Discord::MessageNotificationLevel;

    auto overrideNotif = [this](Snowflake id) -> std::optional<Level> {
        auto it = channelOverrideCache.constFind(id);
        if (it != channelOverrideCache.constEnd() && it->messageNotifications.hasValue() &&
            it->messageNotifications.get() != Level::INHERIT)
            return it->messageNotifications.get();
        return std::nullopt;
    };

    if (auto n = overrideNotif(channelId))
        return *n;
    if (parentId.isValid())
        if (auto n = overrideNotif(parentId))
            return *n;

    auto gs = guildSettingsMap.constFind(guildId);
    if (gs != guildSettingsMap.constEnd() && gs->messageNotifications.hasValue() &&
        gs->messageNotifications.get() != Level::INHERIT)
        return gs->messageNotifications.get();

    auto gi = guildInfo.constFind(guildId);
    if (gi != guildInfo.constEnd())
        return gi->defaultMessageNotifications;

    return Level::ALL_MESSAGES;
}

// Discord's resolveUnreadSetting. Accounts without the newer notification settings light the
// server icon for any unread channel; with them, explicit unread flags on the channel, its
// category and then the guild decide, falling back to the message notification level.
bool ReadStateManager::unreadCountsForGuild(Snowflake guildId, Snowflake channelId, Snowflake parentId) const
{
    if (!useNewNotifications)
        return true;

    using Discord::ChannelOverrideFlag;
    using Discord::UserGuildSettingsFlag;

    auto channelSetting = [this](Snowflake id) -> std::optional<bool> {
        auto it = channelOverrideCache.constFind(id);
        if (it == channelOverrideCache.constEnd())
            return std::nullopt;
        if (hasFlag(it.value(), ChannelOverrideFlag::UNREADS_ALL_MESSAGES))
            return true;
        if (hasFlag(it.value(), ChannelOverrideFlag::UNREADS_ONLY_MENTIONS))
            return false;
        return std::nullopt;
    };

    if (auto s = channelSetting(channelId))
        return *s;
    if (parentId.isValid())
        if (auto s = channelSetting(parentId))
            return *s;

    auto gs = guildSettingsMap.constFind(guildId);
    if (gs != guildSettingsMap.constEnd()) {
        if (hasFlag(gs.value(), UserGuildSettingsFlag::UNREADS_ALL_MESSAGES))
            return true;
        if (hasFlag(gs.value(), UserGuildSettingsFlag::UNREADS_ONLY_MENTIONS))
            return false;
    }

    return resolveMessageNotifications(guildId, channelId, parentId) == Discord::MessageNotificationLevel::ALL_MESSAGES;
}

Snowflake ReadStateManager::guildForChannel(Snowflake channelId) const
{
    auto it = channelGuildMap.constFind(channelId);
    return it != channelGuildMap.constEnd() ? it.value() : Snowflake::Invalid;
}

void ReadStateManager::setGuildReadInfo(Snowflake guildId, const QDateTime &joinedAt,
                                        Discord::MessageNotificationLevel defaultMessageNotifications,
                                        bool isCommunity)
{
    if (!guildId.isValid())
        return;

    GuildReadInfo info;
    info.joinedAtMs = joinedAt.isValid() ? joinedAt.toMSecsSinceEpoch() : 0;
    info.defaultMessageNotifications = defaultMessageNotifications;
    info.isCommunity = isCommunity;
    guildInfo.insert(guildId, info);
}

void ReadStateManager::registerChannelGuild(Snowflake channelId, Snowflake guildId)
{
    if (!channelId.isValid() || !guildId.isValid())
        return;
    channelGuildMap.insert(channelId, guildId);
}

void ReadStateManager::registerChannel(const Discord::Channel &channel, Snowflake guildId)
{
    Snowflake channelId = channel.id.get();
    registerChannelGuild(channelId, guildId);

    if (channel.isVoice())
        voiceChannels.insert(channelId);
    else
        voiceChannels.remove(channelId);

    if (!channel.flags.hasValue())
        return;
    if (channel.flags->testFlag(Discord::ChannelFlag::IS_GUILD_RESOURCE_CHANNEL))
        resourceChannels.insert(channelId);
    else
        resourceChannels.remove(channelId);
}

void ReadStateManager::removeGuild(Snowflake guildId)
{
    const QList<Snowflake> channels = channelGuildMap.keys(guildId);
    for (Snowflake channelId : channels) {
        channelReadStates.remove(channelId);
        channelLastMessageIds.remove(channelId);
        ackIdAtSelect.remove(channelId);
        channelOverrideCache.remove(channelId);
        channelGuildMap.remove(channelId);
        resourceChannels.remove(channelId);
        voiceChannels.remove(channelId);
        outgoingAcks.remove(channelId);
        if (activeChannelId == channelId)
            activeChannelId = Snowflake();
    }

    guildInfo.remove(guildId);
    guildSettingsMap.remove(guildId);
    guildOverrideChannels.remove(guildId);
}

int ReadStateManager::getMentionCount(Snowflake channelId) const
{
    auto it = channelReadStates.constFind(channelId);
    if (it == channelReadStates.constEnd())
        return 0;

    return it->mentionCount.hasValue() ? it->mentionCount.get() : 0;
}

bool ReadStateManager::isChannelMuted(Snowflake channelId) const
{
    auto it = channelOverrideCache.constFind(channelId);
    return it != channelOverrideCache.constEnd() && isMutedSetting(it.value());
}

bool ReadStateManager::isGuildMuted(Snowflake guildId) const
{
    auto it = guildSettingsMap.constFind(guildId);
    return it != guildSettingsMap.constEnd() && isMutedSetting(it.value());
}

bool ReadStateManager::isSuppressEveryone(Snowflake guildId) const
{
    auto it = guildSettingsMap.constFind(guildId);
    return it != guildSettingsMap.constEnd() && it->suppressEveryone.hasValue() && it->suppressEveryone.get();
}

bool ReadStateManager::isSuppressRoles(Snowflake guildId) const
{
    auto it = guildSettingsMap.constFind(guildId);
    return it != guildSettingsMap.constEnd() && it->suppressRoles.hasValue() && it->suppressRoles.get();
}

bool ReadStateManager::isMuteActive(bool muted, const Discord::MuteConfig *muteConfig)
{
    if (!muted)
        return false;

    if (!muteConfig)
        return true;

    if (!muteConfig->endTime.hasValue())
        return true;

    QString endTimeStr = muteConfig->endTime.get();
    if (endTimeStr.isEmpty())
        return true;

    QDateTime endTime = QDateTime::fromString(endTimeStr, Qt::ISODate);
    if (!endTime.isValid())
        return true;

    return QDateTime::currentDateTimeUtc() < endTime;
}

Discord::ReadStateEntry &ReadStateManager::entryFor(Snowflake channelId)
{
    auto it = channelReadStates.find(channelId);
    if (it != channelReadStates.end())
        return it.value();

    Discord::ReadStateEntry entry;
    entry.id = channelId;
    entry.mentionCount = 0;
    return channelReadStates.insert(channelId, entry).value();
}

void ReadStateManager::onMessageAck(const Discord::MessageAck &ack)
{
    Snowflake channelId = ack.channelId.get();
    Snowflake messageId = ack.messageId.get();
    bool manual = ack.manual.hasValue() && ack.manual.get();
    auto &entry = entryFor(channelId);

    if (manual)
        outgoingAcks.remove(channelId);
    else if (entry.lastMessageId.hasValue() && entry.lastMessageId.get() == messageId)
        return;

    entry.lastMessageId = messageId;
    entry.mentionCount = manual && ack.mentionCount.hasValue() ? ack.mentionCount.get() : 0;
    emit readStateUpdated(channelId);
}

void ReadStateManager::onUserGuildSettingsUpdate(const Discord::UserGuildSettings &settings)
{
    Snowflake key = settings.guildId.isNull() ? Snowflake(0) : settings.guildId.get();

    guildSettingsMap.insert(key, settings);
    rebuildChannelOverrideCache(key);

    emit guildSettingsUpdated(key);
}

void ReadStateManager::ackLocally(Snowflake channelId, Snowflake messageId)
{
    auto &entry = entryFor(channelId);
    entry.mentionCount = 0;
    if (messageId.isValid())
        entry.lastMessageId = messageId;
    emit readStateUpdated(channelId);
}

void ReadStateManager::ack(Snowflake channelId, Snowflake messageId, bool immediate)
{
    if (!messageId.isValid())
        messageId = getChannelLastMessageId(channelId);

    bool clearsMentions = getMentionCount(channelId) > 0;
    ackLocally(channelId, messageId);
    if (!messageId.isValid())
        return;

    bool alreadyScheduled = outgoingAcks.contains(channelId);
    outgoingAcks.insert(channelId, messageId);
    if (alreadyScheduled)
        return;

    QTimer::singleShot(immediate || clearsMentions ? 0 : kDeferredAckDelayMs, this, [this, channelId]() { flushOutgoingAck(channelId); });
}

void ReadStateManager::flushOutgoingAck(Snowflake channelId)
{
    auto it = outgoingAcks.find(channelId);
    if (it == outgoingAcks.end())
        return;

    Snowflake messageId = it.value();
    outgoingAcks.erase(it);
    emit ackRequested(channelId, messageId);
}

void ReadStateManager::setActiveChannel(Snowflake channelId)
{
    if (activeChannelId != channelId) {
        if (channelId.isValid())
            ackIdAtSelect.insert(channelId, effectiveAckId(channelId, guildForChannel(channelId)));

        activeChannelId = channelId;
        activeChannelAtBottom = true;
    }

    tryAckActiveChannel();
}

void ReadStateManager::setActiveChannelAtBottom(bool atBottom)
{
    if (activeChannelAtBottom == atBottom)
        return;

    activeChannelAtBottom = atBottom;
    if (atBottom)
        tryAckActiveChannel();
}

bool ReadStateManager::canAutoAckActiveChannel() const
{
    if (!activeChannelId.isValid() || !activeChannelAtBottom)
        return false;
    return !qGuiApp || qGuiApp->applicationState() == Qt::ApplicationActive;
}

void ReadStateManager::tryAckActiveChannel()
{
    if (!canAutoAckActiveChannel() || !hasUnreadOrMentions(activeChannelId))
        return;

    ack(activeChannelId, getChannelLastMessageId(activeChannelId), false);
}

void ReadStateManager::markChannelAsRead(Snowflake channelId, Snowflake lastMessageId)
{
    if (!lastMessageId.isValid() || !hasUnreadOrMentions(channelId, lastMessageId))
        return;

    ack(channelId, lastMessageId, true);
}

void ReadStateManager::markChannelsAsRead(
        const QList<QPair<Snowflake, Snowflake>> &channelMessagePairs)
{
    QList<QPair<Snowflake, Snowflake>> toAck;
    for (const auto &[channelId, messageId] : channelMessagePairs) {
        if (!messageId.isValid() || !hasUnreadOrMentions(channelId, messageId))
            continue;
        outgoingAcks.remove(channelId);
        ackLocally(channelId, messageId);
        toAck.append({ channelId, messageId });
    }

    if (!toAck.isEmpty())
        emit bulkAckRequested(toAck);
}

void ReadStateManager::handleMessageCreated(Snowflake channelId, Snowflake messageId, bool fromSelf, bool isMention)
{
    updateChannelLastMessageId(channelId, messageId);

    if (fromSelf) {
        outgoingAcks.remove(channelId);
        ackLocally(channelId, messageId);
        return;
    }

    if (channelId == activeChannelId && canAutoAckActiveChannel()) {
        ack(channelId, messageId, false);
        return;
    }

    if (!isMention)
        return;

    auto &entry = entryFor(channelId);
    entry.mentionCount = getMentionCount(channelId) + 1;
    emit readStateUpdated(channelId);
}

void ReadStateManager::updateChannelLastMessageId(Snowflake channelId, Snowflake messageId)
{
    if (!messageId.isValid())
        return;

    auto it = channelLastMessageIds.constFind(channelId);
    if (it == channelLastMessageIds.constEnd() || messageId > it.value())
        channelLastMessageIds.insert(channelId, messageId);
}

Snowflake ReadStateManager::getChannelLastMessageId(Snowflake channelId) const
{
    auto it = channelLastMessageIds.constFind(channelId);
    return it != channelLastMessageIds.constEnd() ? it.value() : Snowflake::Invalid;
}

int ReadStateManager::daysSinceDiscordEpoch()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    static const QDateTime epoch = QDateTime(QDate(2015, 1, 1), QTime(0, 0), QTimeZone::UTC);
#else
    static const QDateTime epoch = QDateTime(QDate(2015, 1, 1), QTime(0, 0), Qt::UTC);
#endif
    return epoch.daysTo(QDateTime::currentDateTimeUtc()) + 1;
}

void ReadStateManager::rebuildChannelOverrideCache(Snowflake guildSettingsKey)
{
    auto oldChannels = guildOverrideChannels.take(guildSettingsKey);
    for (const auto &channelId : oldChannels)
        channelOverrideCache.remove(channelId);

    auto it = guildSettingsMap.constFind(guildSettingsKey);
    if (it == guildSettingsMap.constEnd())
        return;

    const auto &settings = it.value();
    if (!settings.channelOverrides.hasValue())
        return;

    QSet<Snowflake> newChannels;
    for (const auto &override_ : settings.channelOverrides.get()) {
        Snowflake channelId = override_.channelId.get();
        channelOverrideCache.insert(channelId, override_);
        newChannels.insert(channelId);
    }
    guildOverrideChannels.insert(guildSettingsKey, newChannels);
}

} // namespace Core
} // namespace Acheron
