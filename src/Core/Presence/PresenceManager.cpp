#include "PresenceManager.hpp"

#include <QDateTime>

#include "ActivityFormat.hpp"
#include "Core/Logging.hpp"

namespace Acheron {
namespace Core {

namespace {

bool sameEntry(const PresenceEntry &a, const PresenceEntry &b)
{
    return a.status == b.status &&
           a.clientStatus.isMobileOnly() == b.clientStatus.isMobileOnly() &&
           a.sortedActivities == b.sortedActivities;
}

bool allOffline(const QHash<Snowflake, PresenceEntry> &guildSlots)
{
    for (const PresenceEntry &entry : guildSlots) {
        if (entry.status != Discord::StatusType::OFFLINE)
            return false;
    }

    return true;
}

} // namespace

PresenceManager::PresenceManager(QObject *parent) : QObject(parent)
{
    flushTimer.setSingleShot(true);
    flushTimer.setInterval(0);
    connect(&flushTimer, &QTimer::timeout, this, &PresenceManager::flushDirty);
}

void PresenceManager::setSelfUserId(Snowflake userId)
{
    selfUserId = userId;
}

void PresenceManager::reset()
{
    for (auto it = presences.cbegin(); it != presences.cend(); ++it)
        markDirty(it.key());

    presences.clear();
    selfPresence = PresenceEntry();
    selfCustomStatus.reset();
    sessionsReplaceSeen = false;
    showCurrentGame = true;
    markDirty(selfUserId);
}

void PresenceManager::markDirty(Snowflake userId)
{
    if (!userId.isValid())
        return;

    dirtyUsers.insert(userId);
    if (!flushTimer.isActive())
        flushTimer.start();
}

void PresenceManager::flushDirty()
{
    if (dirtyUsers.isEmpty())
        return;

    const QList<Snowflake> userIds(dirtyUsers.cbegin(), dirtyUsers.cend());
    dirtyUsers.clear();
    emit presencesChanged(userIds);
}

bool PresenceManager::apply(const Discord::Presence &presence, Snowflake guildId)
{
    const Snowflake userId = presence.userId();
    if (!userId.isValid())
        return false;

    if (userId == selfUserId)
        return false;

    PresenceEntry entry;
    entry.status = presence.statusType();
    if (presence.clientStatus.hasValue())
        entry.clientStatus = presence.clientStatus.get();
    if (presence.activities.hasValue())
        entry.sortedActivities = ActivityFormat::sortAndFilter(presence.activities.get());
    entry.receivedAtMs = QDateTime::currentMSecsSinceEpoch();

    const bool offline = entry.status == Discord::StatusType::OFFLINE;
    if (offline) {
        if (!presences.contains(userId))
            return false;
        entry.sortedActivities.clear();
    }

    QHash<Snowflake, PresenceEntry> &guildSlots = presences[userId];
    auto existing = guildSlots.find(guildId);
    if (existing != guildSlots.end() && sameEntry(existing.value(), entry)) {
        existing->receivedAtMs = entry.receivedAtMs;
        return false;
    }

    guildSlots.insert(guildId, entry);
    if (offline && allOffline(guildSlots))
        presences.remove(userId);

    markDirty(userId);
    return true;
}

void PresenceManager::ingestMember(const Discord::Member &member, Snowflake guildId)
{
    if (!member.presence.hasValue())
        return;

    Discord::Presence presence = member.presence.get();
    if (!presence.userId().isValid()) {
        if (member.user.hasValue()) {
            presence.user = member.user.get();
        } else if (member.userId.hasValue()) {
            Discord::User user;
            user.id = member.userId.get();
            presence.user = user;
        }
    }

    apply(presence, guildId);
}

void PresenceManager::loadMergedPresences(const Discord::MergedPresences &merged, const QList<Snowflake> &guildIdsInOrder)
{
    int count = 0;

    if (merged.friends.hasValue()) {
        for (const Discord::Presence &presence : merged.friends.get()) {
            if (apply(presence, {}))
                count++;
        }
    }

    if (merged.guilds.hasValue()) {
        const QList<QList<Discord::Presence>> &guilds = merged.guilds.get();
        const int guildCount = static_cast<int>(qMin(guilds.size(), guildIdsInOrder.size()));
        for (int i = 0; i < guildCount; i++) {
            for (const Discord::Presence &presence : guilds.at(i)) {
                if (apply(presence, guildIdsInOrder.at(i)))
                    count++;
            }
        }
    }

    qCInfo(LogCore) << "Applied" << count << "presences";
}

void PresenceManager::rebuildSelfActivities(QList<Discord::Activity> sessionActivities)
{
    for (int i = sessionActivities.size() - 1; i >= 0; i--) {
        if (sessionActivities.at(i).isCustom())
            sessionActivities.removeAt(i);
    }

    if (selfCustomStatus.has_value())
        sessionActivities.append(selfCustomStatus.value());

    selfPresence.sortedActivities = ActivityFormat::sortAndFilter(sessionActivities);
}

void PresenceManager::applyProtoStatus(const Proto::PreloadedUserSettings &settings)
{
    const Proto::StatusSettings status = settings.status.has_value() ? settings.status.value() : Proto::StatusSettings();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    showCurrentGame = status.showCurrentGame.value_or(true);

    if (!sessionsReplaceSeen) {
        Discord::StatusType resolved = Discord::StatusType::ONLINE;
        if (status.status.has_value()) {
            resolved = Discord::parseStatus(status.status.value());
            if (resolved == Discord::StatusType::UNKNOWN)
                resolved = Discord::StatusType::ONLINE;
        }

        if (status.statusExpiresAtMs != 0 && now > static_cast<qint64>(status.statusExpiresAtMs))
            resolved = Discord::StatusType::ONLINE;

        selfPresence.status = resolved;
    }

    std::optional<Discord::Activity> custom;
    if (status.customStatus.has_value()) {
        const Proto::CustomStatus &source = status.customStatus.value();
        const bool expired = source.expiresAtMs != 0 && now > static_cast<qint64>(source.expiresAtMs);
        const bool hasEmojiId = source.emojiId != 0;

        if (!expired &&
            (!source.text.isEmpty() || !source.emojiName.isEmpty() || hasEmojiId)) {
            Discord::Activity activity;
            activity.name = QStringLiteral("Custom Status");
            activity.type = Discord::ActivityType::CUSTOM;
            activity.state = source.text;
            activity.createdAt = static_cast<qint64>(source.createdAtMs);

            if (!source.emojiName.isEmpty() || hasEmojiId) {
                Discord::ActivityEmoji emoji;
                if (!source.emojiName.isEmpty())
                    emoji.name = source.emojiName;
                if (hasEmojiId)
                    emoji.id = Core::Snowflake(source.emojiId);
                activity.emoji = emoji;
            }

            custom = activity;
        }
    }

    selfCustomStatus = custom;
    rebuildSelfActivities(selfPresence.sortedActivities);
    markDirty(selfUserId);
}

const PresenceEntry *PresenceManager::newest(Snowflake userId) const
{
    auto it = presences.constFind(userId);
    if (it == presences.constEnd())
        return nullptr;

    const PresenceEntry *best = nullptr;
    for (const PresenceEntry &entry : it.value()) {
        if (best == nullptr) {
            best = &entry;
            continue;
        }

        if (entry.receivedAtMs > best->receivedAtMs ||
            (entry.receivedAtMs == best->receivedAtMs &&
             entry.sortedActivities.size() > best->sortedActivities.size()))
            best = &entry;
    }

    return best;
}

const PresenceEntry *PresenceManager::resolve(Snowflake userId, Snowflake guildId) const
{
    if (userId.isValid() && userId == selfUserId)
        return &selfPresence;

    const PresenceEntry *entry = nullptr;
    if (guildId.isValid()) {
        auto it = presences.constFind(userId);
        if (it != presences.constEnd()) {
            auto slot = it.value().constFind(guildId);
            if (slot != it.value().constEnd())
                entry = &slot.value();
        }
    }

    if (entry == nullptr)
        entry = newest(userId);

    if (entry != nullptr && entry->status == Discord::StatusType::OFFLINE)
        return nullptr;

    return entry;
}

bool PresenceManager::hidesGames(Snowflake userId) const
{
    return userId == selfUserId && !showCurrentGame;
}

PresenceBadge PresenceManager::badge(Snowflake userId, Snowflake guildId) const
{
    const PresenceEntry *entry = resolve(userId, guildId);
    if (entry == nullptr)
        return {};

    PresenceBadge badge;
    badge.status = entry->status;
    badge.mobile = entry->clientStatus.isMobileOnly();

    if (hidesGames(userId))
        return badge;

    for (const Discord::Activity &activity : entry->sortedActivities) {
        if (activity.kind() == Discord::ActivityType::STREAMING) {
            badge.streaming = true;
            break;
        }
    }

    return badge;
}

QList<Discord::Activity> PresenceManager::activities(Snowflake userId, Snowflake guildId) const
{
    const PresenceEntry *entry = resolve(userId, guildId);
    if (entry == nullptr)
        return {};

    if (!hidesGames(userId))
        return entry->sortedActivities;

    QList<Discord::Activity> visible;
    for (const Discord::Activity &activity : entry->sortedActivities) {
        if (activity.isCustom())
            visible.append(activity);
    }

    return visible;
}

const Discord::Activity *PresenceManager::primaryActivity(Snowflake userId,
                                                          Snowflake guildId) const
{
    const PresenceEntry *entry = resolve(userId, guildId);
    if (entry == nullptr)
        return nullptr;

    return hidesGames(userId) ? ActivityFormat::custom(entry->sortedActivities)
                              : ActivityFormat::primary(entry->sortedActivities);
}

void PresenceManager::onPresenceUpdate(const Discord::Presence &presence)
{
    const Snowflake guildId = presence.guildId.hasValue() ? presence.guildId.get() : Snowflake();
    apply(presence, guildId);
}

void PresenceManager::onPresencesReplace(const QList<Discord::Presence> &replacements)
{
    if (replacements.isEmpty())
        return;

    dropSlot({});

    for (const Discord::Presence &presence : replacements)
        apply(presence, {});
}

void PresenceManager::onSessionsReplace(const QList<Discord::UserSession> &sessions)
{
    if (sessions.isEmpty())
        return;

    const Discord::UserSession *chosen = nullptr;
    for (const Discord::UserSession &session : sessions) {
        if (session.isAggregate()) {
            chosen = &session;
            break;
        }
    }

    if (chosen == nullptr) {
        for (const Discord::UserSession &session : sessions) {
            if (session.active.hasValue() && session.active.get()) {
                chosen = &session;
                break;
            }
        }
    }

    if (chosen == nullptr)
        chosen = &sessions.constFirst();

    selfPresence.status = chosen->statusType();
    if (selfPresence.status == Discord::StatusType::UNKNOWN)
        selfPresence.status = Discord::StatusType::ONLINE;
    rebuildSelfActivities(chosen->activities.hasValue() ? chosen->activities.get()
                                                        : QList<Discord::Activity>());

    selfPresence.clientStatus = Discord::ClientStatus();
    selfPresence.receivedAtMs = QDateTime::currentMSecsSinceEpoch();
    sessionsReplaceSeen = true;

    markDirty(selfUserId);
}

void PresenceManager::onGuildCreated(const Discord::GatewayGuild &guild)
{
    if (!guild.presences.hasValue())
        return;

    const Snowflake guildId = guild.properties->id;
    for (const Discord::Presence &presence : guild.presences.get())
        apply(presence, guildId);
}

void PresenceManager::onGuildRemoved(Snowflake guildId)
{
    dropSlot(guildId);
}

void PresenceManager::dropSlot(Snowflake guildId)
{
    for (auto it = presences.begin(); it != presences.end();) {
        if (!it.value().remove(guildId)) {
            ++it;
            continue;
        }

        markDirty(it.key());
        if (it.value().isEmpty())
            it = presences.erase(it);
        else
            ++it;
    }
}

void PresenceManager::onGuildMemberListUpdate(const Discord::GuildMemberListUpdate &update)
{
    if (!update.ops.hasValue())
        return;

    const Snowflake guildId = update.guildId;

    for (const auto &op : update.ops.get()) {
        if (op.items.hasValue()) {
            for (const auto &item : op.items.get()) {
                if (item.member.hasValue())
                    ingestMember(item.member.get(), guildId);
            }
        }

        if (op.item.hasValue() && op.item->member.hasValue())
            ingestMember(op.item->member.get(), guildId);
    }
}

void PresenceManager::onGuildMembersChunk(const Discord::GuildMembersChunk &chunk)
{
    if (!chunk.presences.hasValue())
        return;

    for (const Discord::Presence &presence : chunk.presences.get())
        apply(presence, chunk.guildId);
}

} // namespace Core
} // namespace Acheron
