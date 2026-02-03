#include "PermissionManager.hpp"
#include "PermissionComputer.hpp"
#include "Logging.hpp"

#include "Storage/RoleRepository.hpp"
#include "Storage/GuildRepository.hpp"
#include "Storage/ChannelRepository.hpp"
#include "Storage/MemberRepository.hpp"

namespace Acheron {
namespace Core {

PermissionManager::PermissionManager(Snowflake accountId, QObject *parent)
    : QObject(parent),
      roleRepo(accountId),
      guildRepo(accountId),
      channelRepo(accountId),
      memberRepo(accountId)
{
}

Discord::Permissions PermissionManager::getChannelPermissions(Snowflake userId, Snowflake channelId)
{
    auto cacheKey = qMakePair(userId, channelId);
    if (permissionCache.contains(cacheKey))
        return permissionCache.value(cacheKey);

    auto permissions = computeChannelPermissions(userId, channelId);

    permissionCache.insert(cacheKey, permissions);

    return permissions;
}

bool PermissionManager::hasChannelPermission(Snowflake userId, Snowflake channelId,
                                             Discord::Permissions permission)
{
    auto perms = getChannelPermissions(userId, channelId);
    return (perms & permission) == permission;
}

void PermissionManager::precomputeGuildPermissions(const Discord::Guild &guild,
                                                   const Discord::Member &member,
                                                   const QList<Discord::Role> &roles,
                                                   const QList<Discord::Channel> &channels,
                                                   Snowflake userId)
{
    QList<Snowflake> memberRoleIds;
    if (member.roles.hasValue())
        memberRoleIds = member.roles.get();

    for (const auto &channel : channels) {
        QList<Discord::PermissionOverwrite> overwrites;
        if (channel.permissionOverwrites.hasValue())
            overwrites = channel.permissionOverwrites.get();

        auto permissions = PermissionComputer::computeChannelPermissions(
                guild.ownerId.get(), userId, guild.id.get(), false, memberRoleIds, roles,
                overwrites);

        auto cacheKey = qMakePair(userId, channel.id.get());
        permissionCache.insert(cacheKey, permissions);
    }
}

Discord::Permissions PermissionManager::computeChannelPermissions(Snowflake userId,
                                                                  Snowflake channelId)
{
    auto channelOpt = channelRepo.getChannel(channelId);
    if (!channelOpt) {
        qCWarning(LogCore) << "PermissionManager: Channel not found:" << channelId;
        return Discord::NO_PERMISSIONS;
    }

    const auto &channel = *channelOpt;

    bool isDM = !channel.guildId.hasValue();

    if (isDM) {
        return PermissionComputer::computeChannelPermissions(Snowflake::Invalid, userId,
                                                             Snowflake::Invalid, true, {}, {}, {});
    }

    Snowflake guildId = channel.guildId.get();

    auto guildOpt = guildRepo.getGuild(guildId);
    if (!guildOpt) {
        qCWarning(LogCore) << "PermissionManager: Guild not found:" << guildId;
        return Discord::NO_PERMISSIONS;
    }

    const auto &guild = *guildOpt;

    auto memberOpt = memberRepo.getMember(guildId, userId);
    if (!memberOpt) {
        qCWarning(LogCore) << "PermissionManager: Member not found:" << userId << "in" << guildId;
        return Discord::NO_PERMISSIONS;
    }

    const auto &member = *memberOpt;

    auto allRoles = roleRepo.getRolesForGuild(guildId);

    QList<Discord::PermissionOverwrite> overwrites;
    if (channel.permissionOverwrites.hasValue())
        overwrites = channel.permissionOverwrites.get();

    QList<Snowflake> memberRoleIds;
    if (member.roles.hasValue())
        memberRoleIds = member.roles.get();

    return PermissionComputer::computeChannelPermissions(
            guild.ownerId.get(), userId, guildId, false, memberRoleIds, allRoles, overwrites);
}

void PermissionManager::invalidateChannelCache(Snowflake channelId)
{
    auto it = permissionCache.begin();
    while (it != permissionCache.end()) {
        if (it.key().second == channelId)
            it = permissionCache.erase(it);
        else
            ++it;
    }

    qCDebug(LogCore) << "Invalidated permission cache for channel:" << channelId;
    emit channelPermissionsChanged(channelId);
}

} // namespace Core
} // namespace Acheron
