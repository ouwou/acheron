#include "ReadStateManager.hpp"

#include <QTimeZone>

#include "Core/Logging.hpp"
#include "Core/PermissionManager.hpp"
#include "Discord/Enums.hpp"

namespace Acheron {
namespace Core {

ReadStateManager::ReadStateManager(Snowflake accountId, PermissionManager *perms, QObject *parent)
    : QObject(parent), accountId(accountId), permissionManager(perms)
{
    activeChannelAckTimer.setInterval(10000);
    connect(&activeChannelAckTimer, &QTimer::timeout, this, [this]() {
        if (!activeChannelAckPending || !activeChannelId.isValid())
            return;

        activeChannelAckPending = false;

        auto entry = getReadStateEntry(activeChannelId);
        if (entry && entry->lastMessageId.hasValue())
            emit ackRequested(activeChannelId, entry->lastMessageId.get());
    });
}

void ReadStateManager::loadFromReady(const QList<Discord::ReadStateEntry> &readStates,
                                     const QList<Discord::UserGuildSettings> &guildSettings)
{
    channelReadStates.clear();
    guildSettingsMap.clear();
    guildInfo.clear();
    channelGuildMap.clear();
    ackIdAtSelect.clear();

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

    qCDebug(LogCore) << "ReadStateManager loaded" << channelReadStates.size() << "read states and"
                     << guildSettingsMap.size() << "guild settings";
}

ChannelReadState ReadStateManager::computeChannelReadState(Snowflake channelId, Snowflake guildId,
                                                           Snowflake parentId, bool isDM) const
{
    ChannelReadState result;

    bool canView = isDM || permissionManager->hasChannelPermission(
                                   accountId, channelId, Discord::Permission::VIEW_CHANNEL);

    auto lmIt = channelLastMessageIds.constFind(channelId);
    Snowflake lastMessageId = lmIt != channelLastMessageIds.constEnd() ? lmIt.value()
                                                                       : Snowflake();

    result.isMuted = isChannelMuted(channelId);
    result.mentionCount = canView ? getMentionCount(channelId) : 0;

    bool fullyMuted = result.isMuted ||
                      (parentId.isValid() && isChannelMuted(parentId)) ||
                      (guildId.isValid() && isGuildMuted(guildId));

    result.isUnread = canView && isChannelUnread(channelId, lastMessageId, guildId);

    auto effective = isDM ? Discord::MessageNotificationLevel::ALL_MESSAGES : resolveMessageNotifications(guildId, channelId, parentId);
    result.countsForGuildUnread =
            result.isUnread &&
            !fullyMuted &&
            (result.mentionCount > 0 || effective == Discord::MessageNotificationLevel::ALL_MESSAGES);

    return result;
}

ChannelReadState ReadStateManager::computeThreadReadState(Snowflake threadId, Snowflake guildId,
                                                          Snowflake parentId, bool joined) const
{
    if (joined)
        return computeChannelReadState(threadId, guildId, parentId);

    ChannelReadState result;
    bool canView = permissionManager->hasChannelPermission(accountId, threadId, Discord::Permission::VIEW_CHANNEL);
    result.isMuted = isChannelMuted(threadId);
    result.mentionCount = canView ? getMentionCount(threadId) : 0;

    bool fullyMuted = result.isMuted ||
                      (parentId.isValid() && isChannelMuted(parentId)) ||
                      (guildId.isValid() && isGuildMuted(guildId));

    result.isUnread = result.mentionCount > 0;
    result.countsForGuildUnread = !fullyMuted && result.mentionCount > 0;
    return result;
}

bool ReadStateManager::isChannelUnread(Snowflake channelId, Snowflake channelLastMessageId, Snowflake guildId) const
{
    if (!channelLastMessageId.isValid())
        return false;

    return channelLastMessageId > effectiveAckId(channelId, guildId);
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

    updateLocalReadState(threadId, lastMessageId);
    emit ackRequested(threadId, lastMessageId);
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

Snowflake ReadStateManager::guildForChannel(Snowflake channelId) const
{
    auto it = channelGuildMap.constFind(channelId);
    return it != channelGuildMap.constEnd() ? it.value() : Snowflake::Invalid;
}

void ReadStateManager::setGuildReadInfo(Snowflake guildId, const QDateTime &joinedAt, Discord::MessageNotificationLevel defaultMessageNotifications)
{
    if (!guildId.isValid())
        return;

    GuildReadInfo info;
    info.joinedAtMs = joinedAt.isValid() ? joinedAt.toMSecsSinceEpoch() : 0;
    info.defaultMessageNotifications = defaultMessageNotifications;
    guildInfo.insert(guildId, info);
}

void ReadStateManager::registerChannelGuild(Snowflake channelId, Snowflake guildId)
{
    if (!channelId.isValid() || !guildId.isValid())
        return;
    channelGuildMap.insert(channelId, guildId);
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
    if (it == channelOverrideCache.constEnd())
        return false;

    const auto &override = it.value();
    bool muted = override.muted.hasValue() && override.muted.get();
    const Discord::MuteConfig *mc =
            override.muteConfig.hasValue() ? &override.muteConfig.get() : nullptr;
    return isMuteActive(muted, mc);
}

bool ReadStateManager::isGuildMuted(Snowflake guildId) const
{
    auto it = guildSettingsMap.constFind(guildId);
    if (it == guildSettingsMap.constEnd())
        return false;

    const auto &settings = it.value();
    bool muted = settings.muted.hasValue() && settings.muted.get();
    const Discord::MuteConfig *mc =
            settings.muteConfig.hasValue() ? &settings.muteConfig.get() : nullptr;
    return isMuteActive(muted, mc);
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

void ReadStateManager::onMessageAck(const Discord::MessageAck &ack)
{
    Snowflake channelId = ack.channelId.get();

    auto it = channelReadStates.find(channelId);
    if (it == channelReadStates.end()) {
        Discord::ReadStateEntry entry;
        entry.id = channelId;
        entry.lastMessageId = ack.messageId.get();
        entry.mentionCount = ack.mentionCount.hasValue() ? ack.mentionCount.get() : 0;
        channelReadStates.insert(channelId, entry);
    } else {
        it->lastMessageId = ack.messageId.get();
        if (ack.mentionCount.hasValue())
            it->mentionCount = ack.mentionCount.get();
    }

    emit readStateUpdated(channelId);
}

void ReadStateManager::onUserGuildSettingsUpdate(const Discord::UserGuildSettings &settings)
{
    Snowflake key = settings.guildId.isNull() ? Snowflake(0) : settings.guildId.get();

    guildSettingsMap.insert(key, settings);
    rebuildChannelOverrideCache(key);

    emit guildSettingsUpdated(key);
}

void ReadStateManager::updateLocalReadState(Snowflake channelId, Snowflake lastMessageId)
{
    auto it = channelReadStates.find(channelId);
    if (it == channelReadStates.end()) {
        Discord::ReadStateEntry entry;
        entry.id = channelId;
        entry.lastMessageId = lastMessageId;
        entry.mentionCount = 0;
        channelReadStates.insert(channelId, entry);
    } else {
        it->lastMessageId = lastMessageId;
        it->mentionCount = 0;
    }

    emit readStateUpdated(channelId);
}

void ReadStateManager::setActiveChannel(Snowflake channelId)
{
    if (activeChannelId == channelId)
        return;

    if (channelId.isValid())
        ackIdAtSelect.insert(channelId, effectiveAckId(channelId, guildForChannel(channelId)));

    activeChannelId = channelId;
    activeChannelAckPending = false;

    if (channelId.isValid())
        activeChannelAckTimer.start();
    else
        activeChannelAckTimer.stop();
}

void ReadStateManager::markChannelAsRead(Snowflake channelId, Snowflake lastMessageId)
{
    if (!lastMessageId.isValid())
        return;

    if (!isChannelUnread(channelId, lastMessageId, guildForChannel(channelId)))
        return;

    updateLocalReadState(channelId, lastMessageId);
    emit ackRequested(channelId, lastMessageId);
}

void ReadStateManager::markChannelsAsRead(
        const QList<QPair<Snowflake, Snowflake>> &channelMessagePairs)
{
    if (channelMessagePairs.isEmpty())
        return;

    QList<QPair<Snowflake, Snowflake>> toAck;
    for (const auto &[channelId, messageId] : channelMessagePairs) {
        if (!messageId.isValid())
            continue;
        if (!isChannelUnread(channelId, messageId, guildForChannel(channelId)))
            continue;
        updateLocalReadState(channelId, messageId);
        toAck.append({ channelId, messageId });
    }

    if (!toAck.isEmpty())
        emit bulkAckRequested(toAck);
}

void ReadStateManager::handleMessageCreated(Snowflake channelId, Snowflake messageId,
                                            bool isMention)
{
    updateChannelLastMessageId(channelId, messageId);

    if (channelId == activeChannelId) {
        updateLocalReadState(channelId, messageId);
        activeChannelAckPending = true;
        return;
    }

    if (isMention) {
        auto it = channelReadStates.find(channelId);
        if (it == channelReadStates.end()) {
            Discord::ReadStateEntry entry;
            entry.id = channelId;
            entry.mentionCount = 1;
            channelReadStates.insert(channelId, entry);
        } else {
            int current = it->mentionCount.hasValue() ? it->mentionCount.get() : 0;
            it->mentionCount = current + 1;
        }
        emit readStateUpdated(channelId);
    }
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

const Discord::UserGuildSettings *ReadStateManager::getGuildSettings(Snowflake guildId) const
{
    auto it = guildSettingsMap.constFind(guildId);
    if (it == guildSettingsMap.constEnd())
        return nullptr;
    return &it.value();
}

std::optional<Discord::ReadStateEntry> ReadStateManager::getReadStateEntry(Snowflake channelId) const
{
    auto it = channelReadStates.constFind(channelId);
    if (it == channelReadStates.constEnd())
        return std::nullopt;
    return it.value();
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
    // remove stale entries for this guild
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
