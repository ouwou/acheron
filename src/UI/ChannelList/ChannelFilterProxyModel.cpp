#include "ChannelFilterProxyModel.hpp"
#include "ChannelTreeModel.hpp"
#include "ChannelNode.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/PermissionManager.hpp"
#include "Discord/Enums.hpp"

namespace Acheron {
namespace UI {

ChannelFilterProxyModel::ChannelFilterProxyModel(Core::Session *session, QObject *parent)
    : QSortFilterProxyModel(parent), session(session), channelModel(nullptr)
{
}

void ChannelFilterProxyModel::setSourceModel(QAbstractItemModel *sourceModel)
{
    channelModel = qobject_cast<ChannelTreeModel *>(sourceModel);
    QSortFilterProxyModel::setSourceModel(sourceModel);
}

QVariant ChannelFilterProxyModel::data(const QModelIndex &index, int role) const
{
    if (role == ChannelFilterProxyModel::SelectedRole) {
        QModelIndex sourceIndex = mapToSource(index);
        ChannelNode *node = static_cast<ChannelNode *>(sourceIndex.internalPointer());
        if (node && node->id == selectedChannelId) {
            Core::Snowflake accountId = getUserIdForNode(sourceIndex);
            return accountId == selectedAccountId;
        }
        return false;
    }
    return QSortFilterProxyModel::data(index, role);
}

void ChannelFilterProxyModel::setSelectedChannel(Core::Snowflake channelId, Core::Snowflake accountId)
{
    if (selectedChannelId == channelId && selectedAccountId == accountId)
        return;
    selectedChannelId = channelId;
    selectedAccountId = accountId;
    beginFilterChange();
    endFilterChange();
}

void ChannelFilterProxyModel::invalidateFilter()
{
    beginFilterChange();
    endFilterChange();
}

bool ChannelFilterProxyModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    auto leftType = static_cast<ChannelNode::Type>(left.data(ChannelTreeModel::TypeRole).toInt());
    auto rightType = static_cast<ChannelNode::Type>(right.data(ChannelTreeModel::TypeRole).toInt());

    auto isGuildType = [](ChannelNode::Type t) {
        return t == ChannelNode::Type::Channel || t == ChannelNode::Type::VoiceChannel ||
               t == ChannelNode::Type::Category;
    };

    if (isGuildType(leftType) && isGuildType(rightType)) {
        // sort order: Channel < VoiceChannel < Category
        auto rank = [](ChannelNode::Type t) -> int {
            if (t == ChannelNode::Type::Channel)
                return 0;
            if (t == ChannelNode::Type::VoiceChannel)
                return 1;
            return 2; // Category
        };

        int leftRank = rank(leftType);
        int rightRank = rank(rightType);
        if (leftRank != rightRank)
            return leftRank < rightRank;

        int leftPos = left.data(ChannelTreeModel::PositionRole).toInt();
        int rightPos = right.data(ChannelTreeModel::PositionRole).toInt();
        return leftPos < rightPos;
    }

    if (leftType == ChannelNode::Type::DMChannel && rightType == ChannelNode::Type::DMChannel) {
        quint64 leftMsgId = left.data(ChannelTreeModel::LastMessageIdRole).toULongLong();
        quint64 rightMsgId = right.data(ChannelTreeModel::LastMessageIdRole).toULongLong();
        return leftMsgId > rightMsgId;
    }

    // preserve underlying
    return left.row() < right.row();
}

bool ChannelFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (!channelModel)
        return true;

    QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!index.isValid())
        return true;

    auto nodeType = static_cast<ChannelNode::Type>(index.data(ChannelTreeModel::TypeRole).toInt());

    Core::Snowflake userId = getUserIdForNode(index);
    if (!userId.isValid())
        return true;

    auto *instance = session->client(userId);
    if (!instance)
        return true;

    // hide read channels under collapsed categories, but keep selected channel visible
    if (nodeType == ChannelNode::Type::Channel) {
        ChannelNode *parentNode = static_cast<ChannelNode *>(sourceParent.internalPointer());
        if (parentNode && parentNode->type == ChannelNode::Type::Category && parentNode->collapsed) {
            Core::Snowflake channelId =
                    Core::Snowflake(index.data(ChannelTreeModel::IdRole).toULongLong());
            if (channelId == selectedChannelId && userId == selectedAccountId)
                return true;
            bool unread = index.data(ChannelTreeModel::IsUnreadRole).toBool();
            bool muted = index.data(ChannelTreeModel::IsMutedRole).toBool();
            if (!unread || muted)
                return false;
        }
    }

    // hide voice channels under collapsed categories
    if (nodeType == ChannelNode::Type::VoiceChannel) {
        ChannelNode *parentNode = static_cast<ChannelNode *>(sourceParent.internalPointer());
        if (parentNode && parentNode->type == ChannelNode::Type::Category && parentNode->collapsed)
            return false;
    }

    if (nodeType == ChannelNode::Type::Channel || nodeType == ChannelNode::Type::VoiceChannel) {
        return hasChannelViewPermission(index);
    } else if (nodeType == ChannelNode::Type::Category) {
        auto *permissionManager = instance->permissions();
        if (!permissionManager)
            return true;

        Core::Snowflake channelId =
                Core::Snowflake(index.data(ChannelTreeModel::IdRole).toULongLong());

        if (permissionManager->hasChannelPermission(userId, channelId, Discord::Permission::VIEW_CHANNEL | Discord::Permission::MANAGE_CHANNELS))
            return true;
        return hasVisibleChildren(index);
    }

    return true;
}

bool ChannelFilterProxyModel::hasVisibleChildren(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return false;

    int rows = sourceModel()->rowCount(parent);
    for (int i = 0; i < rows; ++i) {
        QModelIndex child = sourceModel()->index(i, 0, parent);
        if (child.isValid() && hasChannelViewPermission(child))
            return true;
    }

    return false;
}

bool ChannelFilterProxyModel::hasChannelViewPermission(const QModelIndex &sourceIndex) const
{
    Core::Snowflake userId = getUserIdForNode(sourceIndex);
    if (!userId.isValid())
        return true;

    auto *instance = session->client(userId);
    if (!instance)
        return true;

    auto *perms = instance->permissions();
    if (!perms)
        return true;

    Core::Snowflake channelId =
            Core::Snowflake(sourceIndex.data(ChannelTreeModel::IdRole).toULongLong());
    return perms->hasChannelPermission(userId, channelId, Discord::Permission::VIEW_CHANNEL);
}

// todo: uhhh
Core::Snowflake ChannelFilterProxyModel::getUserIdForNode(const QModelIndex &index) const
{
    if (!channelModel)
        return Core::Snowflake::Invalid;

    QModelIndex current = index;
    while (current.isValid()) {
        auto nodeType =
                static_cast<ChannelNode::Type>(current.data(ChannelTreeModel::TypeRole).toInt());
        if (nodeType == ChannelNode::Type::Account)
            return Core::Snowflake(current.data(ChannelTreeModel::IdRole).toULongLong());
        current = current.parent();
    }

    return Core::Snowflake::Invalid;
}

} // namespace UI
} // namespace Acheron
