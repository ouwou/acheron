#include "MemberListManager.hpp"

#include <algorithm>

#include "MurmurHash3.hpp"

// #define MEMBERLIST_DEBUG
#ifdef MEMBERLIST_DEBUG
#  include "Logging.hpp"
#  define ML_LOG qCDebug(LogCore)
#else
#  define ML_LOG QT_NO_QDEBUG_MACRO()
#endif

#include "Storage/ChannelRepository.hpp"
#include "Storage/RoleRepository.hpp"

static uint32_t mmh32(const std::string &input, uint32_t seed)
{
    uint32_t result;
    MurmurHash3_x86_32(input.data(), static_cast<int>(input.size()), seed, &result);
    return result;
}

namespace Acheron {
namespace Core {

MemberListManager::MemberListManager(Storage::ChannelRepository &channelRepo,
                                     Storage::RoleRepository &roleRepo,
                                     QObject *parent)
    : QObject(parent), channelRepo(channelRepo), roleRepo(roleRepo)
{
    responseTimer.setSingleShot(true);
    responseTimer.setInterval(5000);
    connect(&responseTimer, &QTimer::timeout, this, &MemberListManager::flushPendingRanges);
}

GuildListState *MemberListManager::activeGuildState()
{
    if (!activeGuildId.isValid())
        return nullptr;
    auto it = guildStates.find(activeGuildId);
    if (it != guildStates.end())
        return &it.value();
    return nullptr;
}

const GuildListState *MemberListManager::activeGuildState() const
{
    if (!activeGuildId.isValid())
        return nullptr;
    auto it = guildStates.constFind(activeGuildId);
    if (it != guildStates.constEnd())
        return &it.value();
    return nullptr;
}

ListData *MemberListManager::activeListData()
{
    auto *gs = activeGuildState();
    if (!gs || listId.isEmpty())
        return nullptr;
    auto it = gs->lists.find(listId);
    if (it != gs->lists.end())
        return &it.value();
    return nullptr;
}

const ListData *MemberListManager::activeListData() const
{
    const auto *gs = activeGuildState();
    if (!gs || listId.isEmpty())
        return nullptr;
    auto it = gs->lists.constFind(listId);
    if (it != gs->lists.constEnd())
        return &it.value();
    return nullptr;
}

void MemberListManager::setActiveChannel(Snowflake guildId, Snowflake channelId)
{
    ML_LOG << "[ML] setActiveChannel guild:" << guildId
           << "ch:" << channelId
           << "(was guild:" << activeGuildId
           << "ch:" << activeChannelId
           << "list:" << listId << ")";

    if (activeGuildId == guildId && activeChannelId == channelId)
        return;

    bool sameGuild = (activeGuildId == guildId);

    responseTimer.stop();
    awaitingResponse = false;
    hasPendingRanges = false;

    activeGuildId = guildId;
    activeChannelId = channelId;

    GuildListState &gs = guildStates[activeGuildId];

    if (gs.roleCache.isEmpty()) {
        const auto roles = roleRepo.getRolesForGuild(activeGuildId);
        for (const auto &role : roles)
            gs.roleCache.insert(QString::number(role.id.get()), role);
        ML_LOG << "[ML] loaded" << gs.roleCache.size() << "roles";
    }

    auto overwrites = channelRepo.getPermissionOverwrites(activeChannelId);
    Discord::Permissions everyonePerms;
    auto rit = gs.roleCache.find(QString::number(activeGuildId));
    if (rit != gs.roleCache.end())
        everyonePerms = rit->permissions;
    QString newListId = computeListId(overwrites, everyonePerms);

    bool listChanged = (newListId != listId) || !sameGuild;
    listId = newListId;

    ListData &ld = gs.lists[newListId];
    ML_LOG << "[ML] listId:" << newListId
           << "sameGuild:" << sameGuild
           << "listChanged:" << listChanged
           << "cached items:" << ld.items.size()
           << "cached groups:" << ld.groups.size();

    // always [0, 99]
    ranges = { { 0, 99 } };

    if (listChanged) {
        emit listAboutToReset();
        emit listReset();
    }

    awaitingResponse = true;
    responseTimer.start();
    emit subscriptionRequested(activeGuildId, activeChannelId, ranges);
}

void MemberListManager::clear()
{
    ML_LOG << "[ML] clear (was guild:" << activeGuildId << "list:" << listId << ")";
    responseTimer.stop();
    awaitingResponse = false;
    hasPendingRanges = false;

    emit listAboutToReset();
    activeGuildId = Snowflake::Invalid;
    activeChannelId = Snowflake::Invalid;
    listId.clear();
    ranges.clear();
    emit listReset();
}

void MemberListManager::handleRoleCreated(Snowflake guildId, const Discord::Role &role)
{
    auto it = guildStates.find(guildId);
    if (it == guildStates.end())
        return;

    QString roleIdStr = QString::number(role.id.get());
    it->roleCache.insert(roleIdStr, role);
}

void MemberListManager::handleRoleUpdated(Snowflake guildId, const Discord::Role &role)
{
    auto it = guildStates.find(guildId);
    if (it == guildStates.end())
        return;

    GuildListState &gs = it.value();
    QString roleIdStr = QString::number(role.id.get());
    gs.roleCache.insert(roleIdStr, role);

    bool changed = false;
    for (auto lit = gs.lists.begin(); lit != gs.lists.end(); ++lit) {
        ListData &ld = lit.value();
        for (auto iit = ld.items.begin(); iit != ld.items.end(); ++iit) {
            auto &item = iit.value();
            if (item.type == MemberListItem::Type::Group && item.groupId == roleIdStr) {
                resolveGroupInfo(item, gs, ld);
                changed = true;
            } else if (item.type == MemberListItem::Type::Member && !item.member.roles.isUndefined()) {
                for (const auto &rid : item.member.roles.get()) {
                    if (QString::number(rid) == roleIdStr) {
                        resolveMemberInfo(item, gs);
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    if (changed && guildId == this->activeGuildId) {
        emit listAboutToReset();
        emit listReset();
    }
}

void MemberListManager::handleRoleDeleted(Snowflake guildId, Snowflake roleId)
{
    auto it = guildStates.find(guildId);
    if (it == guildStates.end())
        return;

    GuildListState &gs = it.value();
    QString roleIdStr = QString::number(roleId);
    gs.roleCache.remove(roleIdStr);

    bool changed = false;
    for (auto lit = gs.lists.begin(); lit != gs.lists.end(); ++lit) {
        ListData &ld = lit.value();
        for (auto iit = ld.items.begin(); iit != ld.items.end(); ++iit) {
            auto &item = iit.value();
            if (item.type == MemberListItem::Type::Member && !item.member.roles.isUndefined()) {
                for (const auto &rid : item.member.roles.get()) {
                    if (QString::number(rid) == roleIdStr) {
                        resolveMemberInfo(item, gs);
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    if (changed && guildId == this->activeGuildId) {
        emit listAboutToReset();
        emit listReset();
    }
}

void MemberListManager::handleMemberListUpdate(const Discord::GuildMemberListUpdate &update)
{
    ML_LOG << "[ML] update guild:" << update.guildId.get()
           << "list:" << update.id
           << "ops:" << update.ops->size()
           << "groups:" << update.groups->size()
           << "(active guild:" << activeGuildId
           << "list:" << listId << ")";

    GuildListState &gs = guildStates[update.guildId];
    gs.memberCount = update.memberCount;
    gs.onlineCount = update.onlineCount;

    ListData &ld = gs.lists[update.id];

    if (!update.groups->isEmpty())
        ld.groups = update.groups;

    bool isActiveList = (update.guildId == activeGuildId && update.id == listId);

    if (isActiveList) {
        awaitingResponse = false;
        responseTimer.stop();
    }

    if (isActiveList)
        emit listAboutToReset();

    for (const auto &op : update.ops.get()) {
        const QString &opType = op.op;

        if (opType == "SYNC") {
            ML_LOG << "[ML]  SYNC [" << (op.range.isUndefined() ? -1 : op.range->first)
                   << "-" << (op.range.isUndefined() ? -1 : op.range->second)
                   << "] items:" << (op.items.isUndefined() ? -1 : op.items->size());
            processSync(op, ld, gs);
        } else if (opType == "INSERT") {
            ML_LOG << "[ML]  INSERT idx:" << (op.index.isUndefined() ? -1 : (int)op.index);
            processInsert(op, ld, gs);
        } else if (opType == "UPDATE") {
            ML_LOG << "[ML]  UPDATE idx:" << (op.index.isUndefined() ? -1 : (int)op.index);
            processUpdate(op, ld, gs);
        } else if (opType == "DELETE") {
            ML_LOG << "[ML]  DELETE idx:" << (op.index.isUndefined() ? -1 : (int)op.index);
            processDelete(op, ld);
        } else if (opType == "INVALIDATE") {
            ML_LOG << "[ML]  INVALIDATE [" << (op.range.isUndefined() ? -1 : op.range->first)
                   << "-" << (op.range.isUndefined() ? -1 : op.range->second) << "]";
            processInvalidate(op, ld);
        }
    }

    ML_LOG << "[ML] after: list:" << update.id
           << "items:" << ld.items.size()
           << "groups:" << ld.groups.size()
           << "isActive:" << isActiveList
           << "totalItemCount:" << (isActiveList ? totalItemCount() : -1);

    if (isActiveList)
        emit listReset();

    if (isActiveList && hasPendingRanges && pendingRanges != ranges)
        applyAndSendRanges(pendingRanges);
    else if (isActiveList)
        hasPendingRanges = false;
}

int MemberListManager::totalItemCount() const
{
    const auto *ld = activeListData();
    if (!ld)
        return 0;
    int count = 0;
    for (const auto &group : ld->groups)
        count += 1 + group.count;
    return count;
}

const MemberListItem *MemberListManager::itemAt(int index) const
{
    const auto *ld = activeListData();
    if (!ld)
        return nullptr;
    auto it = ld->items.constFind(index);
    if (it != ld->items.constEnd())
        return &it.value();
    return nullptr;
}

bool MemberListManager::isLoaded(int index) const
{
    const auto *ld = activeListData();
    return ld && ld->items.contains(index);
}

int MemberListManager::totalMemberCount() const
{
    const auto *gs = activeGuildState();
    return gs ? gs->memberCount : 0;
}

int MemberListManager::onlineCount() const
{
    const auto *gs = activeGuildState();
    return gs ? gs->onlineCount : 0;
}

void MemberListManager::updateSubscriptionRange(int firstVisible, int lastVisible)
{
    if (!activeGuildId.isValid() || !activeChannelId.isValid())
        return;

    // always [0, 99]
    QList<QPair<int, int>> newRanges = { { 0, 99 } };

    if (lastVisible > 99) {
        int firstRangeStart = (firstVisible / 100) * 100;
        if (firstRangeStart > 0)
            newRanges.append({ firstRangeStart, firstRangeStart + 99 });

        int lastRangeStart = (lastVisible / 100) * 100;
        if (lastRangeStart != firstRangeStart && lastRangeStart > 0) {
            if (newRanges.size() < 3)
                newRanges.append({ lastRangeStart, lastRangeStart + 99 });
        }
    }

    if (newRanges == ranges && !hasPendingRanges)
        return;

    if (awaitingResponse) {
        pendingRanges = newRanges;
        hasPendingRanges = true;
        return;
    }

    if (newRanges == ranges)
        return;

    applyAndSendRanges(newRanges);
}

void MemberListManager::applyAndSendRanges(const QList<QPair<int, int>> &newRanges)
{
    QList<QPair<int, int>> oldRanges = ranges;
    ranges = newRanges;
    hasPendingRanges = false;

    evictUnsubscribedItems(oldRanges, newRanges);

    awaitingResponse = true;
    responseTimer.start();
    emit subscriptionRequested(activeGuildId, activeChannelId, ranges);
}

void MemberListManager::flushPendingRanges()
{
    awaitingResponse = false;

    if (hasPendingRanges && pendingRanges != ranges)
        applyAndSendRanges(pendingRanges);
    else
        hasPendingRanges = false;
}

void MemberListManager::evictUnsubscribedItems(const QList<QPair<int, int>> &,
                                               const QList<QPair<int, int>> &newRanges)
{
    auto *ld = activeListData();
    if (!ld)
        return;

    QList<int> toRemove;
    for (auto it = ld->items.constBegin(); it != ld->items.constEnd(); ++it) {
        if (!indexInRanges(it.key(), newRanges))
            toRemove.append(it.key());
    }

    if (toRemove.isEmpty())
        return;

    emit listAboutToReset();
    for (int idx : toRemove)
        ld->items.remove(idx);
    emit listReset();
}

bool MemberListManager::indexInRanges(int index, const QList<QPair<int, int>> &ranges)
{
    for (const auto &r : ranges) {
        if (index >= r.first && index <= r.second)
            return true;
    }
    return false;
}

MemberListItem MemberListManager::syncItemToListItem(
        const Discord::GuildMemberListUpdate::SyncItem &syncItem,
        const GuildListState &guildState, const ListData &listData)
{
    MemberListItem listItem;

    if (!syncItem.group.isUndefined() && syncItem.group->id.hasValue()) {
        listItem.type = MemberListItem::Type::Group;
        listItem.groupId = syncItem.group->id;
        listItem.groupCount = syncItem.group->count;
        resolveGroupInfo(listItem, guildState, listData);
    } else if (!syncItem.member.isUndefined()) {
        listItem.type = MemberListItem::Type::Member;
        listItem.member = syncItem.member;

        if (!syncItem.member->user.isUndefined())
            listItem.userId = syncItem.member->user->id;

        resolveMemberInfo(listItem, guildState);
    }

    return listItem;
}

void MemberListManager::resolveGroupInfo(MemberListItem &item, const GuildListState &guildState, const ListData &listData)
{
    for (const auto &group : listData.groups) {
        if (group.id.get() == item.groupId) {
            item.groupCount = group.count;
            break;
        }
    }

    if (item.groupId == "online") {
        item.groupName = "Online";
        item.groupColor = QColor();
    } else if (item.groupId == "offline") {
        item.groupName = "Offline";
        item.groupColor = QColor();
    } else {
        auto it = guildState.roleCache.find(item.groupId);
        if (it != guildState.roleCache.end()) {
            item.groupName = it->name;
            if (it->hasColor())
                item.groupColor = it->getColor();
        } else {
            item.groupName = "Unknown Role";
        }
    }
}

void MemberListManager::resolveMemberInfo(MemberListItem &item, const GuildListState &guildState)
{
    const auto &member = item.member;

    if (!member.nick.isUndefined() && !member.nick.isNull() && !member.nick->isEmpty())
        item.displayName = member.nick;
    else if (!member.user.isUndefined())
        item.displayName = member.user->getDisplayName();

    if (!member.roles.isUndefined()) {
        int highestPos = -1;
        int bestColor = 0;

        for (const auto &roleId : member.roles.get()) {
            QString roleIdStr = QString::number(roleId);
            auto it = guildState.roleCache.find(roleIdStr);
            if (it != guildState.roleCache.end()) {
                if (!it->color.isUndefined() && it->color.get() != 0 && it->position.get() > highestPos) {
                    highestPos = it->position.get();
                    bestColor = it->color.get();
                }
            }
        }

        if (bestColor != 0)
            item.roleColor = QColor((bestColor >> 16) & 0xFF, (bestColor >> 8) & 0xFF, bestColor & 0xFF);
    }
}

void MemberListManager::processSync(const Discord::GuildMemberListUpdate::ListOp &op,
                                    ListData &listData, const GuildListState &guildState)
{
    if (op.range.isUndefined())
        return;

    // no changes
    if (op.items.isUndefined() || op.items->isEmpty())
        return;

    int rangeStart = op.range->first;

    int idx = rangeStart;
    for (const auto &syncItem : op.items.get()) {
        listData.items[idx] = syncItemToListItem(syncItem, guildState, listData);
        ++idx;
    }
}

void MemberListManager::processInsert(const Discord::GuildMemberListUpdate::ListOp &op,
                                      ListData &listData, const GuildListState &guildState)
{
    if (op.index.isUndefined() || op.item.isUndefined())
        return;

    int insertIdx = op.index;

    QList<int> keys;
    keys.reserve(listData.items.size());
    for (auto it = listData.items.constBegin(); it != listData.items.constEnd(); ++it) {
        if (it.key() >= insertIdx)
            keys.append(it.key());
    }

    std::sort(keys.begin(), keys.end(), std::greater<int>());
    for (int key : keys)
        listData.items[key + 1] = listData.items.take(key);

    listData.items[insertIdx] = syncItemToListItem(op.item, guildState, listData);
}

void MemberListManager::processUpdate(const Discord::GuildMemberListUpdate::ListOp &op,
                                      ListData &listData, const GuildListState &guildState)
{
    if (op.index.isUndefined() || op.item.isUndefined())
        return;

    int index = op.index;
    listData.items[index] = syncItemToListItem(op.item, guildState, listData);
}

void MemberListManager::processDelete(const Discord::GuildMemberListUpdate::ListOp &op,
                                      ListData &listData)
{
    if (op.index.isUndefined())
        return;

    int deleteIdx = op.index;
    listData.items.remove(deleteIdx);

    QList<int> keys;
    keys.reserve(listData.items.size());
    for (auto it = listData.items.constBegin(); it != listData.items.constEnd(); ++it) {
        if (it.key() > deleteIdx)
            keys.append(it.key());
    }

    std::sort(keys.begin(), keys.end());
    for (int key : keys)
        listData.items[key - 1] = listData.items.take(key);
}

void MemberListManager::processInvalidate(const Discord::GuildMemberListUpdate::ListOp &op,
                                          ListData &listData)
{
    if (op.range.isUndefined())
        return;

    int rangeStart = op.range->first;
    int rangeEnd = op.range->second;

    for (int i = rangeStart; i <= rangeEnd; i++)
        listData.items.remove(i);
}

QString MemberListManager::computeListId(const QList<Discord::PermissionOverwrite> &overwrites, Discord::Permissions everyonePermissions)
{
    bool hasDenyRules = false;
    for (const auto &ow : overwrites) {
        if (ow.deny->testFlag(Discord::Permission::VIEW_CHANNEL)) {
            hasDenyRules = true;
            break;
        }
    }

    if (!hasDenyRules && everyonePermissions.testFlag(Discord::Permission::VIEW_CHANNEL))
        return QStringLiteral("everyone");

    QStringList hashInputs;
    for (const auto &ow : overwrites) {
        if (ow.allow->testFlag(Discord::Permission::VIEW_CHANNEL))
            hashInputs.append("allow:" + QString::number(ow.id.get()));
        else if (ow.deny->testFlag(Discord::Permission::VIEW_CHANNEL))
            hashInputs.append("deny:" + QString::number(ow.id.get()));
    }

    hashInputs.sort();

    std::string input = hashInputs.join(",").toStdString();
    uint32_t hash = mmh32(input, 0);

    return QString::number(hash);
}

} // namespace Core
} // namespace Acheron
