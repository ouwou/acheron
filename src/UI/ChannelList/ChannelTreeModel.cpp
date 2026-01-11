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
            [this](const QUrl &url, const QSize &size, const QPixmap &pixmap) {
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
            const QSize desiredSize(64, 64);
            QUrl TEMPORARY = QUrl(QString("https://cdn.discordapp.com/icons/%1/%2.png?size=%3")
                                          .arg(quint64(node->id))
                                          .arg(node->TEMP_iconHash)
                                          .arg(desiredSize.width()));
            QPixmap pixmap = session->getImageManager()->get(TEMPORARY, desiredSize);

            if (!session->getImageManager()->isCached(TEMPORARY, desiredSize)) {
                bool alreadyWaiting = false;
                auto it = pendingRequests.constFind(TEMPORARY);
                while (it != pendingRequests.cend() && it.key() == TEMPORARY) {
                    if (it.value() == index) {
                        alreadyWaiting = true;
                        break;
                    }
                    it++;
                }

                if (!alreadyWaiting)
                    pendingRequests.insert(TEMPORARY, QPersistentModelIndex(index));
            }

            return pixmap;
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

    // Folders should be enabled (for expansion) but not selectable
    if (node->type == ChannelNode::Type::Folder)
        return f | Qt::ItemIsEnabled;

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
    if (!accNode)
        return;

    QModelIndex accIndex = indexForNode(accNode);

    auto *instance = session->client(ready.user->id);
    if (!instance)
        return;

    const auto &settings = instance->discord()->getSettings();

    QHash<Core::Snowflake, const Discord::GatewayGuild *> guildMap;
    for (const auto &guild : ready.guilds.get())
        guildMap.insert(guild.properties->id, &guild);

    // folders and unfolder'd guilds
    std::vector<std::unique_ptr<ChannelNode>> topLevelNodes;

    if (settings.guildFolders.has_value() && !settings.guildFolders->folders.isEmpty()) {
        const auto &folders = settings.guildFolders->folders;

        QSet<Core::Snowflake> guildIdsInFolders;
        for (const auto &folder : folders)
            for (const auto &guildId : folder.guildIds)
                guildIdsInFolders.insert(guildId);

        std::vector<Core::Snowflake> unfolderedGuilds;
        for (const auto &guild : ready.guilds.get())
            if (!guildIdsInFolders.contains(guild.properties->id))
                unfolderedGuilds.push_back(guild.properties->id);

        // discord does unfolder'd guilds by id descending
        std::sort(unfolderedGuilds.begin(), unfolderedGuilds.end(),
                  [](const auto &a, const auto &b) { return a > b; });

        // and they come first
        for (const auto &guildId : unfolderedGuilds)
            if (guildMap.contains(guildId))
                topLevelNodes.push_back(createGuildNode(*guildMap[guildId]));

        for (const auto &folder : folders) {
            if (!folder.id.has_value()) {
                // folders with null ids are just guilds
                for (const auto &guildId : folder.guildIds)
                    if (guildMap.contains(guildId))
                        topLevelNodes.push_back(createGuildNode(*guildMap[guildId]));
            } else {
                auto folderNode = std::make_unique<ChannelNode>();
                folderNode->type = ChannelNode::Type::Folder;
                folderNode->name = folder.name.value_or("Unnamed Folder");
                folderNode->folderName = folder.name;
                folderNode->folderColor = folder.color;
                folderNode->id = Core::Snowflake(folder.id.value());

                for (const auto &guildId : folder.guildIds)
                    if (guildMap.contains(guildId))
                        folderNode->addChild(createGuildNode(*guildMap[guildId]));

                topLevelNodes.push_back(std::move(folderNode));
            }
        }
    } else {
        // probably not going to correspond
        for (const auto &guild : ready.guilds.get())
            topLevelNodes.push_back(createGuildNode(guild));
    }

    if (topLevelNodes.empty())
        return;

    int startRow = accNode->children.size();
    int endRow = startRow + topLevelNodes.size() - 1;

    beginInsertRows(accIndex, startRow, endRow);
    for (auto &node : topLevelNodes)
        accNode->addChild(std::move(node));
    endInsertRows();
}

ChannelNode *ChannelTreeModel::getAccountNodeFor(ChannelNode *node)
{
    ChannelNode *accountNode = node;
    while (accountNode && accountNode->type != ChannelNode::Type::Account)
        accountNode = accountNode->parent;
    return accountNode;
}

std::unique_ptr<ChannelNode> ChannelTreeModel::createGuildNode(const Discord::GatewayGuild &guild)
{
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
                if (categoryMap.contains(channel.parentId.get()))
                    categoryMap[channel.parentId.get()]->addChild(std::move(node));
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

    return guildNode;
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