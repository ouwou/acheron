#include "ReadStateManager.hpp"

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
                                                           bool isDM) const
{
    ChannelReadState result;

    bool canView = isDM || permissionManager->hasChannelPermission(
                                   accountId, channelId, Discord::Permission::VIEW_CHANNEL);
    bool canSend = isDM || permissionManager->hasChannelPermission(
                                   accountId, channelId, Discord::Permission::SEND_MESSAGES);

    auto lmIt = channelLastMessageIds.constFind(channelId);
    Snowflake lastMessageId = lmIt != channelLastMessageIds.constEnd() ? lmIt.value()
                                                                       : Snowflake();

    result.isMuted = isChannelMuted(channelId);
    result.isUnread = canView && isChannelUnread(channelId, lastMessageId, canSend);
    result.mentionCount = canView ? getMentionCount(channelId) : 0;

    return result;
}

bool ReadStateManager::isChannelUnread(Snowflake channelId, Snowflake channelLastMessageId,
                                       bool canSendMessages) const
{
    if (!channelLastMessageId.isValid())
        return false;

    auto it = channelReadStates.constFind(channelId);
    if (it == channelReadStates.constEnd())
        // best effort at emulating official behavior. i think i saw an edge case or something idk
        // no read state entry and SEND_MESSAGES means unread otherwise read. or something
        return canSendMessages;

    const auto &entry = it.value();
    if (!entry.lastMessageId.hasValue() || entry.lastMessageId.isNull())
        return true;

    return entry.lastMessageId.get() < channelLastMessageId;
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
            (override.muteConfig.hasValue() && !override.muteConfig.isNull())
                    ? &override.muteConfig.get()
                    : nullptr;
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
            (settings.muteConfig.hasValue() && !settings.muteConfig.isNull())
                    ? &settings.muteConfig.get()
                    : nullptr;
    return isMuteActive(muted, mc);
}

bool ReadStateManager::isMuteActive(bool muted, const Discord::MuteConfig *muteConfig)
{
    if (!muted)
        return false;

    if (!muteConfig)
        return true;

    if (!muteConfig->endTime.hasValue() || muteConfig->endTime.isNull())
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

    if (!isChannelUnread(channelId, lastMessageId))
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
        bool canSend = permissionManager->hasChannelPermission(
                accountId, channelId, Discord::Permission::SEND_MESSAGES);
        if (!isChannelUnread(channelId, messageId, canSend))
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
    static const QDateTime epoch = QDateTime(QDate(2015, 1, 1), QTime(0, 0), QTimeZone::UTC);
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
