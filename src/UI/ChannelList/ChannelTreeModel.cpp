#include "ChannelTreeModel.hpp"

#include <Core/ClientInstance.hpp>
#include <Core/Logging.hpp>
#include <Storage/DatabaseManager.hpp>
#include <Storage/ChannelRepository.hpp>

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

    if (role == IdRole)
        return static_cast<quint64>(node->id);
    if (role == UnreadCountRole)
        return node->unreadCount;
    if (role == TypeRole)
        return static_cast<int>(node->type);
    if (role == PositionRole)
        return node->position;

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
            node->parentId =
                    channel.parentId.hasValue() ? channel.parentId.get() : Core::Snowflake();
            categoryMap[channel.id] = node.get();
            categories.push_back(std::move(node));
        }
    }

    for (const auto &channel : guild.channels.get()) {
        if (channel.type == Discord::ChannelType::GUILD_TEXT ||
            channel.type == Discord::ChannelType::GUILD_NEWS) {
            auto node = std::make_unique<ChannelNode>();
            node->id = channel.id;
            node->name = channel.name;
            node->type = ChannelNode::Type::Channel;
            node->position = channel.position;
            node->parentId =
                    channel.parentId.hasValue() ? channel.parentId.get() : Core::Snowflake();

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

ChannelNode *ChannelTreeModel::findChannelTreeNode(Snowflake channelId, ChannelNode *searchRoot)
{
    if (!searchRoot)
        return nullptr;

    if (searchRoot->type != ChannelNode::Type::Server && searchRoot->id == channelId)
        return searchRoot;

    for (const auto &child : searchRoot->children) {
        if (ChannelNode *found = findChannelTreeNode(channelId, child.get()))
            return found;
    }

    return nullptr;
}

ChannelNode *ChannelTreeModel::findGuildNode(ChannelNode *node)
{
    while (node && node->type != ChannelNode::Type::Server)
        node = node->parent;
    return node;
}

ChannelNode *ChannelTreeModel::findCategoryNode(Snowflake categoryId, ChannelNode *guildNode)
{
    if (!guildNode || guildNode->type != ChannelNode::Type::Server)
        return nullptr;

    for (const auto &child : guildNode->children)
        if (child->type == ChannelNode::Type::Category && child->id == categoryId)
            return child.get();

    return nullptr;
}

ChannelNode *ChannelTreeModel::findGuildNodeById(Snowflake guildId, ChannelNode *accountNode)
{
    if (!accountNode || accountNode->type != ChannelNode::Type::Account)
        return nullptr;

    for (const auto &child : accountNode->children) {
        if (child->type == ChannelNode::Type::Server && child->id == guildId)
            return child.get();

        if (child->type == ChannelNode::Type::Folder) {
            for (const auto &folderChild : child->children)
                if (folderChild->type == ChannelNode::Type::Server && folderChild->id == guildId)
                    return folderChild.get();
        }
    }

    return nullptr;
}

void ChannelTreeModel::addChannel(const Discord::ChannelCreate &event, Snowflake accountId)
{
    if (!event.channel.hasValue())
        return;

    const auto &channel = event.channel.get();

    if (channel.type != Discord::ChannelType::GUILD_TEXT &&
        channel.type != Discord::ChannelType::GUILD_NEWS &&
        channel.type != Discord::ChannelType::GUILD_CATEGORY)
        return;

    if (!channel.guildId.hasValue())
        return;

    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    Snowflake guildId = channel.guildId.get();
    ChannelNode *guildNode = findGuildNodeById(guildId, accNode);
    if (!guildNode)
        return;

    if (findChannelTreeNode(channel.id, guildNode))
        return;

    auto node = std::make_unique<ChannelNode>();
    node->id = channel.id;
    node->name = channel.name;
    node->position = channel.position;
    node->parentId = channel.parentId.hasValue() ? channel.parentId.get() : Core::Snowflake();

    if (channel.type == Discord::ChannelType::GUILD_CATEGORY) {
        node->type = ChannelNode::Type::Category;

        int insertRow = guildNode->children.size();
        QModelIndex guildIdx = indexForNode(guildNode);

        beginInsertRows(guildIdx, insertRow, insertRow);
        guildNode->addChild(std::move(node));
        endInsertRows();
    } else {
        node->type = ChannelNode::Type::Channel;

        ChannelNode *parentNode = nullptr;
        if (node->parentId.isValid())
            parentNode = findCategoryNode(node->parentId, guildNode);
        if (!parentNode)
            parentNode = guildNode;

        int insertRow = parentNode->children.size();
        QModelIndex parentIdx = indexForNode(parentNode);

        beginInsertRows(parentIdx, insertRow, insertRow);
        parentNode->addChild(std::move(node));
        endInsertRows();

        // notify proxy to re-check category visibility
        if (parentNode->type == ChannelNode::Type::Category && parentIdx.isValid())
            emit dataChanged(parentIdx, parentIdx);
    }
}

void ChannelTreeModel::updateChannel(const Discord::ChannelUpdate &update, Snowflake accountId)
{
    const auto &channel = update.channel.get();

    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channel.id, accNode);
    if (!channelNode)
        return;

    auto *instance = session->client(accountId);
    if (!instance)
        return;

    Core::Snowflake oldParentId = channelNode->parentId;
    Core::Snowflake newParentId =
            channel.parentId.hasValue() ? channel.parentId.get() : Core::Snowflake();

    bool parentChanged = oldParentId != newParentId;

    if (parentChanged) {
        ChannelNode *oldParent = channelNode->parent;
        ChannelNode *guildNode = findGuildNode(channelNode);

        if (!guildNode)
            return;

        QModelIndex oldParentIdx = indexForNode(oldParent);
        int oldRow = -1;
        for (size_t i = 0; i < oldParent->children.size(); ++i) {
            if (oldParent->children[i].get() == channelNode) {
                oldRow = i;
                break;
            }
        }

        if (oldRow == -1)
            return;

        beginRemoveRows(oldParentIdx, oldRow, oldRow);
        auto node = std::move(oldParent->children[oldRow]);
        oldParent->children.erase(oldParent->children.begin() + oldRow);
        endRemoveRows();

        node->name = channel.name.get();
        node->position = channel.position.get();
        node->parentId = newParentId;

        ChannelNode *newParent = nullptr;
        if (newParentId.isValid())
            newParent = findCategoryNode(newParentId, guildNode);
        else
            newParent = guildNode;

        if (!newParent) {
            qCWarning(LogUI) << "Could not find new parent for channel:" << channel.id.get();
            return;
        }

        // throw it wherever cuz the proxy will sort it
        int insertRow = newParent->children.size();

        QModelIndex newParentIdx = indexForNode(newParent);
        beginInsertRows(newParentIdx, insertRow, insertRow);
        node->parent = newParent;
        newParent->children.push_back(std::move(node));
        endInsertRows();

        // notify proxy to re-check category visibility
        if (oldParent->type == ChannelNode::Type::Category && oldParentIdx.isValid())
            emit dataChanged(oldParentIdx, oldParentIdx);
        if (newParent->type == ChannelNode::Type::Category && newParentIdx.isValid())
            emit dataChanged(newParentIdx, newParentIdx);
    } else {
        channelNode->name = channel.name.get();
        channelNode->position = channel.position.get();
        channelNode->parentId = newParentId;

        QModelIndex idx = indexForNode(channelNode);
        if (idx.isValid())
            emit dataChanged(idx, idx, { Qt::DisplayRole, PositionRole });

        qCDebug(LogUI) << "Updated channel tree node:" << channel.id.get();
    }
}

void ChannelTreeModel::deleteChannel(const Discord::ChannelDelete &event, Snowflake accountId)
{
    if (!event.id.hasValue())
        return;

    Core::Snowflake channelId = event.id.get();

    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channelId, accNode);
    if (!channelNode)
        return;

    ChannelNode *parent = channelNode->parent;
    if (!parent)
        return;

    QModelIndex parentIdx = indexForNode(parent);
    int row = -1;
    for (size_t i = 0; i < parent->children.size(); ++i) {
        if (parent->children[i].get() == channelNode) {
            row = i;
            break;
        }
    }

    if (row == -1)
        return;

    beginRemoveRows(parentIdx, row, row);
    parent->children.erase(parent->children.begin() + row);
    endRemoveRows();

    // notify proxy to re-check category visibility
    if (parent->type == ChannelNode::Type::Category && parentIdx.isValid())
        emit dataChanged(parentIdx, parentIdx);

    qCDebug(LogUI) << "Deleted channel tree node:" << channelId;
}

} // namespace UI
} // namespace Acheron