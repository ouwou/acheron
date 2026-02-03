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

void ChannelFilterProxyModel::invalidateFilter()
{
    QSortFilterProxyModel::invalidateFilter();
}

bool ChannelFilterProxyModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    auto leftType = static_cast<ChannelNode::Type>(left.data(ChannelTreeModel::TypeRole).toInt());
    auto rightType = static_cast<ChannelNode::Type>(right.data(ChannelTreeModel::TypeRole).toInt());

    if ((leftType == ChannelNode::Type::Channel || leftType == ChannelNode::Type::Category) &&
        (rightType == ChannelNode::Type::Channel || rightType == ChannelNode::Type::Category)) {

        if (leftType == ChannelNode::Type::Channel && rightType == ChannelNode::Type::Category)
            return true;
        else if (leftType == ChannelNode::Type::Category && rightType == ChannelNode::Type::Channel)
            return false;

        int leftPos = left.data(ChannelTreeModel::PositionRole).toInt();
        int rightPos = right.data(ChannelTreeModel::PositionRole).toInt();
        return leftPos < rightPos;
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

    if (nodeType == ChannelNode::Type::Channel || nodeType == ChannelNode::Type::Category) {
        auto *permissionManager = instance->permissions();
        if (!permissionManager)
            return true;

        Core::Snowflake channelId =
                Core::Snowflake(index.data(ChannelTreeModel::IdRole).toULongLong());

        if (nodeType == ChannelNode::Type::Channel) {
            return permissionManager->hasChannelPermission(userId, channelId,
                                                           Discord::Permission::VIEW_CHANNEL);
        } else {
            if (nodeType == ChannelNode::Type::Category) {
                if (permissionManager->hasChannelPermission(userId, channelId, Discord::Permission::VIEW_CHANNEL | Discord::Permission::MANAGE_CHANNELS))
					return true;
                return hasVisibleChildren(index);
            }
        }
    }

    return true;
}

bool ChannelFilterProxyModel::hasVisibleChildren(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return false;

    int rowCount = sourceModel()->rowCount(parent);
    for (int i = 0; i < rowCount; ++i) {
        if (filterAcceptsRow(i, parent))
            return true;
    }

    return false;
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
