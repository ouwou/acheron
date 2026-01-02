#include "ChannelTreeModel.hpp"
#include <Core/ClientInstance.hpp>

namespace Acheron {
namespace UI {
ChannelTreeModel::ChannelTreeModel(Session *session, QObject *parent)
    : QAbstractItemModel(parent), session(session)
{
    root = std::make_unique<ChannelNode>();
    root->name = "Root";
    root->type = ChannelNode::Type::Root;

    connect(session, &Session::accountDetailsUpdated, this, [this](const Core::AccountInfo &info) {
        ChannelNode *node = accountNodes.value(info.id, nullptr);
        if (!node)
            return;

        node->name = info.displayName;

        QModelIndex index = indexForNode(node);
        emit dataChanged(index, index, { Qt::DisplayRole });
    });

    connect(session->getImageManager(), &Core::ImageManager::imageFetched, this,
            [this](const QUrl &url, const QPixmap &pixmap) {
                auto values = pendingRequests.values(url);
                for (const auto &index : values) {
                    if (index.isValid())
                        emit dataChanged(index, index, { Qt::DecorationRole });
                }
            });
}

QModelIndex ChannelTreeModel::index(int row, int column, const QModelIndex &parentIndex) const
{
    if (!hasIndex(row, column, parentIndex))
        return {};

    ChannelNode *parentNode = nodeFromIndex(parentIndex);
    if (row >= (int)parentNode->children.size())
        return {};

    return createIndex(row, column, parentNode->children[row].get());
}

QModelIndex ChannelTreeModel::parent(const QModelIndex &childIndex) const
{
    if (!childIndex.isValid())
        return {};

    ChannelNode *node = nodeFromIndex(childIndex);
    ChannelNode *parentNode = node->parent;

    if (!parentNode || parentNode == root.get())
        return {};

    return indexForNode(parentNode);
}

int ChannelTreeModel::rowCount(const QModelIndex &parentIndex) const
{
    ChannelNode *parentNode = nodeFromIndex(parentIndex);
    return parentNode ? parentNode->children.size() : 0;
}

int ChannelTreeModel::columnCount(const QModelIndex &) const
{
    return 1;
}

QVariant ChannelTreeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    ChannelNode *node = nodeFromIndex(index);

    if (role == Qt::DisplayRole)
        return node->name;

    if (role == Qt::DecorationRole) {
        if (node->type == ChannelNode::Type::Server) {
            // resolve
            QUrl TEMPORARY = QUrl(QString("https://cdn.discordapp.com/icons/%1/%2.png?size=64")
                                          .arg(quint64(node->id))
                                          .arg(node->TEMP_iconHash));
            if (session->getImageManager()->isCached(TEMPORARY)) {
                return session->getImageManager()->get(TEMPORARY);
            } else {
                pendingRequests.insert(TEMPORARY, QPersistentModelIndex(index));
                session->getImageManager()->request(TEMPORARY);
                return session->getImageManager()->placeholder();
            }
        }
        return {};
    }

    if (role == Qt::UserRole)
        return (qulonglong)node->id;
    if (role == Qt::UserRole + 1)
        return node->unreadCount;
    if (role == Qt::UserRole + 2)
        return static_cast<int>(node->type);

    return {};
}

Qt::ItemFlags ChannelTreeModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    ChannelNode *node = nodeFromIndex(index);
    Qt::ItemFlags f = QAbstractItemModel::flags(index);

    if (node->type == ChannelNode::Type::Channel)
        return f | Qt::ItemIsSelectable | Qt::ItemIsEnabled;

    return f & ~Qt::ItemIsSelectable;
}

void ChannelTreeModel::addAccount(const Acheron::Core::AccountInfo &account)
{
    if (accountNodes.contains(account.id))
        return;

    auto *instance = session->client(account.id);

    beginInsertRows({}, root->children.size(), root->children.size());

    auto accNode = std::make_unique<ChannelNode>();
    accNode->id = account.id;
    accNode->name = account.displayName.isEmpty() ? account.username : account.displayName;
    accNode->type = ChannelNode::Type::Account;

    auto dmNode = std::make_unique<ChannelNode>();
    dmNode->name = "Direct Messages";
    dmNode->type = ChannelNode::Type::DMHeader;
    accNode->addChild(std::move(dmNode));

    accountNodes[account.id] = root->addChild(std::move(accNode));

    endInsertRows();
}

void ChannelTreeModel::removeAccount(Snowflake accountId)
{
    if (!accountNodes.contains(accountId))
        return;

    ChannelNode *nodeToRemove = accountNodes[accountId];
    QModelIndex idx = indexForNode(nodeToRemove);

    if (idx.isValid()) {
        beginRemoveRows(QModelIndex(), idx.row(), idx.row());
        root->children.erase(root->children.begin() + idx.row());
        accountNodes.remove(accountId);
        endRemoveRows();
    }
}

void ChannelTreeModel::populateFromReady(const Discord::Ready &ready)
{
    ChannelNode *accNode = accountNodes.value(ready.user->id, nullptr);
    QModelIndex accIndex = indexForNode(accNode);

    int startRow = accNode->children.size();
    int count = ready.guilds->size();
    if (count == 0)
        return;

    beginInsertRows(accIndex, startRow, startRow + count - 1);

    for (const auto &guild : ready.guilds.get()) {
        auto guildNode = std::make_unique<ChannelNode>();
        guildNode->id = guild.properties->id;
        guildNode->name = guild.properties->name;
        guildNode->type = ChannelNode::Type::Server;
        guildNode->TEMP_iconHash = guild.properties->icon;

        QHash<Snowflake, ChannelNode *> categoryMap;
        std::vector<std::unique_ptr<ChannelNode>> categories;
        std::vector<std::unique_ptr<ChannelNode>> orphanChannels;

        for (const auto &channel : guild.channels.get()) {
            if (channel.type == Discord::ChannelType::GUILD_CATEGORY) {
                auto node = std::make_unique<ChannelNode>();
                node->id = channel.id;
                node->name = channel.name;
                node->type = ChannelNode::Type::Category;
                node->position = channel.position;
                categoryMap[channel.id] = node.get();
                categories.push_back(std::move(node));
            }
        }

        for (const auto &channel : guild.channels.get()) {
            if (channel.type == Discord::ChannelType::GUILD_TEXT) {
                auto node = std::make_unique<ChannelNode>();
                node->id = channel.id;
                node->name = channel.name;
                node->type = ChannelNode::Type::Channel;
                node->position = channel.position;

                if (channel.parentId.hasValue() && channel.parentId->isValid()) {
                    if (categoryMap.contains(channel.parentId.get())) {
                        categoryMap[channel.parentId.get()]->addChild(std::move(node));
                    }
                } else {
                    orphanChannels.push_back(std::move(node));
                }
            }
        }

        auto sorter = [](const auto &a, const auto &b) { return a->position < b->position; };

        std::sort(categories.begin(), categories.end(), sorter);
        std::sort(orphanChannels.begin(), orphanChannels.end(), sorter);
        for (const auto &category : categories)
            std::sort(category->children.begin(), category->children.end(), sorter);

        for (auto &node : orphanChannels)
            guildNode->addChild(std::move(node));
        for (auto &node : categories)
            guildNode->addChild(std::move(node));

        accNode->addChild(std::move(guildNode));
    }

    endInsertRows();
}

ChannelNode *ChannelTreeModel::getAccountNodeFor(ChannelNode *node)
{
    ChannelNode *accountNode = node;
    while (accountNode && accountNode->type != ChannelNode::Type::Account)
        accountNode = accountNode->parent;
    return accountNode;
}

ChannelNode *ChannelTreeModel::nodeFromIndex(const QModelIndex &index) const
{
    if (!index.isValid())
        return root.get();
    return static_cast<ChannelNode *>(index.internalPointer());
}
QModelIndex ChannelTreeModel::indexForNode(ChannelNode *node) const
{
    if (!node || node == root.get())
        return {};

    ChannelNode *parent = node->parent;
    if (!parent)
        parent = root.get();

    for (size_t i = 0; i < parent->children.size(); ++i) {
        if (parent->children[i].get() == node) {
            return createIndex(i, 0, node);
        }
    }
    return {};
}
} // namespace UI
} // namespace Acheron