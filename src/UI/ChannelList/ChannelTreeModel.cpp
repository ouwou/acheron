#include "ChannelTreeModel.hpp"

#include <Core/ClientInstance.hpp>
#include <Core/ReadStateManager.hpp>
#include <Core/Logging.hpp>
#include <Discord/CdnUrls.hpp>
#include <Discord/Enums.hpp>
#include <Storage/DatabaseManager.hpp>
#include <Storage/ChannelRepository.hpp>
#include <Storage/UserRepository.hpp>

#ifndef ACHERON_NO_VOICE
#  include <Core/AV/VoiceManager.hpp>
#endif

namespace Acheron {
namespace UI {

static bool isChannelPrivate(const Discord::Channel &channel, Core::Snowflake guildId)
{
    if (!channel.permissionOverwrites.hasValue())
        return false;

    for (const auto &ow : channel.permissionOverwrites.get()) {
        if (ow.type.get() == Discord::PermissionOverwrite::Type::Role && ow.id.get() == guildId)
            return ow.deny->testFlag(Discord::Permission::VIEW_CHANNEL);
    }
    return false;
}

static QString getDMDisplayName(const Discord::Channel &channel, Storage::UserRepository &userRepo)
{
    if (channel.name.hasValue() && !channel.name->isEmpty())
        return channel.name.get();

    QStringList names;

    if (channel.recipients.hasValue()) {
        for (const auto &user : channel.recipients.get())
            names.append(user.getDisplayName());
    } else if (channel.recipientIds.hasValue()) {
        for (const auto &userId : channel.recipientIds.get()) {
            auto userOpt = userRepo.getUser(userId);
            if (userOpt.has_value())
                names.append(userOpt->getDisplayName());
        }
    }

    return names.isEmpty() ? "Unnamed" : names.join(", ");
}
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
            const QSize desiredSize(64, 64);
            QUrl TEMPORARY = Discord::Cdn::guildIcon(node->id, node->TEMP_iconHash,
                                                     desiredSize.width());
            QPixmap pixmap = session->getImageManager()->get(TEMPORARY, desiredSize, Core::PinGroup::ChannelList);

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

        if (node->type == ChannelNode::Type::DMChannel) {
            const QSize desiredSize(64, 64);
            QUrl avatarUrl;

            if (node->dmRecipientId.isValid() && !node->dmAvatarHash.isEmpty()) {
                avatarUrl = Discord::Cdn::userAvatar(node->dmRecipientId, node->dmAvatarHash,
                                                     desiredSize.width());
            } else if (!node->TEMP_iconHash.isEmpty()) {
                avatarUrl = Discord::Cdn::channelIcon(node->id, node->TEMP_iconHash,
                                                      desiredSize.width());
            }

            if (!avatarUrl.isEmpty()) {
                QPixmap pixmap = session->getImageManager()->get(avatarUrl, desiredSize, Core::PinGroup::ChannelList);

                if (!session->getImageManager()->isCached(avatarUrl, desiredSize)) {
                    bool alreadyWaiting = false;
                    auto it = pendingRequests.constFind(avatarUrl);
                    while (it != pendingRequests.cend() && it.key() == avatarUrl) {
                        if (it.value() == index) {
                            alreadyWaiting = true;
                            break;
                        }
                        it++;
                    }

                    if (!alreadyWaiting)
                        pendingRequests.insert(avatarUrl, QPersistentModelIndex(index));
                }

                return pixmap;
            }
        }

        if (node->type == ChannelNode::Type::VoiceParticipant &&
            node->dmRecipientId.isValid() && !node->dmAvatarHash.isEmpty()) {
            const QSize desiredSize(32, 32);
            QUrl avatarUrl = Discord::Cdn::userAvatar(node->dmRecipientId, node->dmAvatarHash,
                                                      desiredSize.width());

            QPixmap pixmap = session->getImageManager()->get(avatarUrl, desiredSize, Core::PinGroup::ChannelList);

            if (!session->getImageManager()->isCached(avatarUrl, desiredSize)) {
                bool alreadyWaiting = false;
                auto it = pendingRequests.constFind(avatarUrl);
                while (it != pendingRequests.cend() && it.key() == avatarUrl) {
                    if (it.value() == index) {
                        alreadyWaiting = true;
                        break;
                    }
                    it++;
                }

                if (!alreadyWaiting)
                    pendingRequests.insert(avatarUrl, QPersistentModelIndex(index));
            }

            return pixmap;
        }

        return {};
    }

    if (role == IdRole)
        return static_cast<quint64>(node->id);
    if (role == TypeRole)
        return static_cast<int>(node->type);
    if (role == PositionRole)
        return node->position;
    if (role == LastMessageIdRole)
        return static_cast<quint64>(node->lastMessageId);
    if (role == IsUnreadRole)
        return node->isUnread;
    if (role == MentionCountRole)
        return node->mentionCount;
    if (role == IsMutedRole)
        return node->isMuted;
    if (role == CollapsedRole)
        return node->collapsed;
    if (role == VoiceParticipantCountRole)
        return node->voiceParticipantCount;
    if (role == UserLimitRole)
        return node->userLimit;

    if (role == IsVoiceMutedRole || role == IsVoiceDeafenedRole) {
        if (node->type != ChannelNode::Type::VoiceParticipant)
            return false;
#ifndef ACHERON_NO_VOICE
        ChannelNode *accNode = getAccountNodeFor(node);
        if (!accNode)
            return false;
        auto *instance = session->client(accNode->id);
        if (!instance)
            return false;
        auto state = instance->voice()->voiceStateForUser(node->id);
        if (!state.has_value())
            return false;
        if (role == IsVoiceMutedRole)
            return state->selfMute.get() || state->mute.get();
        return state->selfDeaf.get() || state->deaf.get();
#else
        return false;
#endif
    }

    return {};
}

Qt::ItemFlags ChannelTreeModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    ChannelNode *node = nodeFromIndex(index);
    Qt::ItemFlags f = QAbstractItemModel::flags(index);

    if (node->type == ChannelNode::Type::Channel || node->type == ChannelNode::Type::DMChannel)
        return f | Qt::ItemIsSelectable | Qt::ItemIsEnabled;

    // Voice channels: enabled (for context menu) but not selectable
    if (node->type == ChannelNode::Type::VoiceChannel)
        return (f | Qt::ItemIsEnabled) & ~Qt::ItemIsSelectable;

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
                if (folder.color != Discord::BLURPLE)
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

    if (ready.privateChannels.hasValue() && !ready.privateChannels->isEmpty()) {
        ChannelNode *dmHeader = nullptr;
        for (const auto &child : accNode->children) {
            if (child->type == ChannelNode::Type::DMHeader) {
                dmHeader = child.get();
                break;
            }
        }

        if (!dmHeader)
            return;

        Storage::UserRepository userRepo(ready.user->id);

        const auto &dms = ready.privateChannels.get();

        QModelIndex dmHeaderIndex = indexForNode(dmHeader);
        int dmStartRow = 0;
        int dmEndRow = dms.size() - 1;

        beginInsertRows(dmHeaderIndex, dmStartRow, dmEndRow);
        for (const auto &channel : dms) {
            auto dmNode = std::make_unique<ChannelNode>();
            dmNode->id = channel.id;
            dmNode->type = ChannelNode::Type::DMChannel;
            dmNode->name = getDMDisplayName(channel, userRepo);
            dmNode->lastMessageId = channel.lastMessageId.hasValue()
                                            ? channel.lastMessageId.get()
                                            : channel.id.get();

            if (channel.recipients.hasValue()) {
                for (const auto &user : channel.recipients.get())
                    dmNode->recipientIds.append(user.id.get());
            } else if (channel.recipientIds.hasValue()) {
                dmNode->recipientIds = channel.recipientIds.get();
            }

            if (channel.type == Discord::ChannelType::DM && dmNode->recipientIds.size() == 1) {
                dmNode->dmRecipientId = dmNode->recipientIds.first();

                if (channel.recipients.hasValue() && !channel.recipients->isEmpty()) {
                    const auto &user = channel.recipients->first();
                    if (user.avatar.hasValue())
                        dmNode->dmAvatarHash = user.avatar.get();
                }

                if (dmNode->dmAvatarHash.isEmpty()) {
                    auto userOpt = userRepo.getUser(dmNode->dmRecipientId);
                    if (userOpt.has_value() && userOpt->avatar.hasValue())
                        dmNode->dmAvatarHash = userOpt->avatar.get();
                }
            } else if (channel.type == Discord::ChannelType::GROUP_DM) {
                if (channel.icon.hasValue())
                    dmNode->TEMP_iconHash = channel.icon.get();
            }

            dmHeader->addChild(std::move(dmNode));
        }
        endInsertRows();
    }

    initChannelReadStates(accNode, instance);
    recomputeSubtreeAggregates(accNode);
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
        bool isText = channel.type == Discord::ChannelType::GUILD_TEXT ||
                      channel.type == Discord::ChannelType::GUILD_NEWS;
        bool isVoice = channel.type == Discord::ChannelType::GUILD_VOICE ||
                       channel.type == Discord::ChannelType::GUILD_STAGE_VOICE;

        if (isText || isVoice) {
            auto node = std::make_unique<ChannelNode>();
            node->id = channel.id;
            node->name = channel.name;
            node->type = isVoice ? ChannelNode::Type::VoiceChannel : ChannelNode::Type::Channel;
            node->position = channel.position;
            node->parentId =
                    channel.parentId.hasValue() ? channel.parentId.get() : Core::Snowflake();
            node->isPrivate = isChannelPrivate(channel, guild.properties->id);
            if (isText) {
                node->lastMessageId = channel.lastMessageId.hasValue()
                                              ? channel.lastMessageId.get()
                                              : Core::Snowflake();
            }
            if (isVoice && channel.userLimit.hasValue())
                node->userLimit = channel.userLimit.get();

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

ChannelNode *ChannelTreeModel::findChannelTreeNode(Snowflake channelId)
{
    return findChannelTreeNode(channelId, root.get());
}

ChannelNode *ChannelTreeModel::findChannelTreeNode(Snowflake channelId, Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return nullptr;
    return findChannelTreeNode(channelId, accNode);
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

    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    if (channel.type == Discord::ChannelType::DM || channel.type == Discord::ChannelType::GROUP_DM) {
        ChannelNode *dmHeader = nullptr;
        for (const auto &child : accNode->children) {
            if (child->type == ChannelNode::Type::DMHeader) {
                dmHeader = child.get();
                break;
            }
        }

        if (!dmHeader)
            return;

        if (findChannelTreeNode(channel.id, dmHeader))
            return;

        Storage::UserRepository userRepo(accountId);

        auto dmNode = std::make_unique<ChannelNode>();
        dmNode->id = channel.id;
        dmNode->type = ChannelNode::Type::DMChannel;
        dmNode->name = getDMDisplayName(channel, userRepo);
        dmNode->lastMessageId = channel.lastMessageId.hasValue()
                                        ? channel.lastMessageId.get()
                                        : channel.id.get();

        if (channel.recipients.hasValue()) {
            for (const auto &user : channel.recipients.get())
                dmNode->recipientIds.append(user.id.get());
        } else if (channel.recipientIds.hasValue()) {
            dmNode->recipientIds = channel.recipientIds.get();
        }

        if (channel.type == Discord::ChannelType::DM && dmNode->recipientIds.size() == 1) {
            dmNode->dmRecipientId = dmNode->recipientIds.first();

            if (channel.recipients.hasValue() && !channel.recipients->isEmpty()) {
                const auto &user = channel.recipients->first();
                if (user.avatar.hasValue())
                    dmNode->dmAvatarHash = user.avatar.get();
            }

            if (dmNode->dmAvatarHash.isEmpty()) {
                auto userOpt = userRepo.getUser(dmNode->dmRecipientId);
                if (userOpt.has_value() && userOpt->avatar.hasValue())
                    dmNode->dmAvatarHash = userOpt->avatar.get();
            }
        } else if (channel.type == Discord::ChannelType::GROUP_DM) {
            if (channel.icon.hasValue())
                dmNode->TEMP_iconHash = channel.icon.get();
        }

        QModelIndex dmHeaderIndex = indexForNode(dmHeader);
        beginInsertRows(dmHeaderIndex, 0, 0);
        dmNode->parent = dmHeader;
        dmHeader->children.insert(dmHeader->children.begin(), std::move(dmNode));
        endInsertRows();

        return;
    }

    if (channel.type != Discord::ChannelType::GUILD_TEXT &&
        channel.type != Discord::ChannelType::GUILD_NEWS &&
        channel.type != Discord::ChannelType::GUILD_VOICE &&
        channel.type != Discord::ChannelType::GUILD_STAGE_VOICE &&
        channel.type != Discord::ChannelType::GUILD_CATEGORY)
        return;

    if (!channel.guildId.hasValue())
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
    if (channel.lastMessageId.hasValue())
        node->lastMessageId = channel.lastMessageId.get();

    if (channel.type == Discord::ChannelType::GUILD_CATEGORY) {
        node->type = ChannelNode::Type::Category;

        int insertRow = guildNode->children.size();
        QModelIndex guildIdx = indexForNode(guildNode);

        beginInsertRows(guildIdx, insertRow, insertRow);
        guildNode->addChild(std::move(node));
        endInsertRows();
    } else {
        bool isVoice = channel.type == Discord::ChannelType::GUILD_VOICE ||
                       channel.type == Discord::ChannelType::GUILD_STAGE_VOICE;
        node->type = isVoice ? ChannelNode::Type::VoiceChannel : ChannelNode::Type::Channel;
        node->isPrivate = isChannelPrivate(channel, guildId);
        if (isVoice && channel.userLimit.hasValue())
            node->userLimit = channel.userLimit.get();

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
        node->isPrivate = isChannelPrivate(channel, guildNode->id);
        if (node->type == ChannelNode::Type::VoiceChannel && channel.userLimit.hasValue())
            node->userLimit = channel.userLimit.get();

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

        ChannelNode *guildNode = findGuildNode(channelNode);
        if (guildNode)
            channelNode->isPrivate = isChannelPrivate(channel, guildNode->id);

        if (channelNode->type == ChannelNode::Type::VoiceChannel && channel.userLimit.hasValue())
            channelNode->userLimit = channel.userLimit.get();

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

void ChannelTreeModel::invalidateGuildData(Snowflake guildId)
{
    for (auto it = accountNodes.begin(); it != accountNodes.end(); ++it) {
        ChannelNode *accountNode = it.value();
        ChannelNode *guildNode = findGuildNodeById(guildId, accountNode);
        if (guildNode) {
            QModelIndex guildIdx = indexForNode(guildNode);
            if (guildIdx.isValid()) {
                emitDataChangedRecursive(guildIdx);
            }
        }
    }
}

void ChannelTreeModel::updateReadState(Snowflake channelId, Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channelId, accNode);
    if (!channelNode)
        return;

    auto *instance = session->client(accountId);
    if (!instance)
        return;

    // determine guildId for this channel
    ChannelNode *guildNode = findGuildNode(channelNode);
    Snowflake guildId = guildNode ? guildNode->id : Snowflake::Invalid;

    bool wasMuted = channelNode->isMuted;
    bool wasUnread = channelNode->isUnread;
    int oldMentions = channelNode->mentionCount;

    bool isDM = channelNode->type == ChannelNode::Type::DMChannel;
    auto state = instance->readState()->computeChannelReadState(channelNode->id, guildId, isDM);
    applyChannelReadState(channelNode, state);

    if (channelNode->isMuted != wasMuted || channelNode->isUnread != wasUnread ||
        channelNode->mentionCount != oldMentions) {
        QModelIndex idx = indexForNode(channelNode);
        if (idx.isValid())
            emit dataChanged(idx, idx, { IsUnreadRole, MentionCountRole, IsMutedRole });

        // propagate changes upward through the tree
        if (channelNode->parent)
            updateNodeAggregates(channelNode->parent);
    }
}

void ChannelTreeModel::updateGuildSettings(Snowflake guildId, Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    auto *instance = session->client(accountId);
    if (!instance)
        return;

    ChannelNode *targetNode = nullptr;
    if (guildId == Snowflake(0)) {
        for (const auto &child : accNode->children) {
            if (child->type == ChannelNode::Type::DMHeader) {
                targetNode = child.get();
                break;
            }
        }
    } else {
        targetNode = findGuildNodeById(guildId, accNode);
    }

    if (!targetNode)
        return;

    if (targetNode->type == ChannelNode::Type::Server) {
        targetNode->isMuted = instance->readState()->isGuildMuted(guildId);
        QModelIndex idx = indexForNode(targetNode);
        if (idx.isValid())
            emit dataChanged(idx, idx, { IsMutedRole });
    }

    updateChildrenReadState(targetNode, guildId, instance);
    updateNodeAggregates(targetNode);
}

void ChannelTreeModel::applyChannelReadState(ChannelNode *node, const Core::ChannelReadState &state)
{
    node->isUnread = state.isUnread;
    node->mentionCount = state.mentionCount;
    node->isMuted = state.isMuted;
}

static bool isContainerType(ChannelNode::Type type)
{
    return type == ChannelNode::Type::Category || type == ChannelNode::Type::Server ||
           type == ChannelNode::Type::Folder;
}

void ChannelTreeModel::aggregateChildren(ChannelNode *node)
{
    node->mentionCount = 0;
    node->isUnread = false;
    for (const auto &child : node->children) {
        if (child->isUnread && !child->isMuted)
            node->isUnread = true;
        node->mentionCount += child->mentionCount;
    }
}

void ChannelTreeModel::recomputeSubtreeAggregates(ChannelNode *node)
{
    for (const auto &child : node->children)
        recomputeSubtreeAggregates(child.get());

    if (isContainerType(node->type))
        aggregateChildren(node);
}

void ChannelTreeModel::updateNodeAggregates(ChannelNode *node)
{
    if (!node || !isContainerType(node->type))
        return;

    int oldMentionCount = node->mentionCount;
    bool oldIsUnread = node->isUnread;

    aggregateChildren(node);

    if (oldMentionCount != node->mentionCount || oldIsUnread != node->isUnread) {
        QModelIndex idx = indexForNode(node);
        if (idx.isValid())
            emit dataChanged(idx, idx, { IsUnreadRole, MentionCountRole });

        if (node->parent)
            updateNodeAggregates(node->parent);
    }
}

void ChannelTreeModel::initChannelReadStates(ChannelNode *node, Core::ClientInstance *instance)
{
    if (node->type == ChannelNode::Type::Server)
        node->isMuted = instance->readState()->isGuildMuted(node->id);

    if (node->type == ChannelNode::Type::Channel ||
        node->type == ChannelNode::Type::DMChannel) {
        ChannelNode *guildNode = findGuildNode(node);
        Snowflake guildId = guildNode ? guildNode->id : Snowflake::Invalid;
        bool isDM = node->type == ChannelNode::Type::DMChannel;
        auto state = instance->readState()->computeChannelReadState(node->id, guildId, isDM);
        applyChannelReadState(node, state);
    }

    for (const auto &child : node->children)
        initChannelReadStates(child.get(), instance);
}

void ChannelTreeModel::updateChildrenReadState(ChannelNode *node, Snowflake guildId,
                                               Core::ClientInstance *instance)
{
    for (const auto &child : node->children) {
        if (child->type == ChannelNode::Type::Channel ||
            child->type == ChannelNode::Type::DMChannel) {
            bool isDM = child->type == ChannelNode::Type::DMChannel;
            auto state = instance->readState()->computeChannelReadState(child->id, guildId, isDM);
            applyChannelReadState(child.get(), state);

            QModelIndex idx = indexForNode(child.get());
            if (idx.isValid())
                emit dataChanged(idx, idx, { IsUnreadRole, MentionCountRole, IsMutedRole });
        }

        if (!child->children.empty())
            updateChildrenReadState(child.get(), guildId, instance);
    }
}

void ChannelTreeModel::emitDataChangedRecursive(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    emit dataChanged(index, index);

    int rows = rowCount(index);
    for (int i = 0; i < rows; ++i) {
        QModelIndex child = this->index(i, 0, index);
        if (child.isValid())
            emitDataChangedRecursive(child);
    }
}

void ChannelTreeModel::toggleCollapsed(const QModelIndex &index)
{
    ChannelNode *node = nodeFromIndex(index);
    if (!node)
        return;

    setCollapsed(index, !node->collapsed);
}

void ChannelTreeModel::setCollapsed(const QModelIndex &index, bool collapsed)
{
    ChannelNode *node = nodeFromIndex(index);
    if (!node || node->collapsed == collapsed)
        return;

    node->collapsed = collapsed;
    emit dataChanged(index, index, { CollapsedRole });
}

void ChannelTreeModel::updateChannelLastMessageId(Snowflake channelId, Snowflake messageId,
                                                  Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channelId, accNode);
    if (!channelNode)
        return;

    if (channelNode->lastMessageId.isValid() && messageId <= channelNode->lastMessageId)
        return;

    channelNode->lastMessageId = messageId;
    updateReadState(channelId, accountId);
}

void ChannelTreeModel::updateVoiceCount(Snowflake channelId, int count, Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channelId, accNode);
    if (!channelNode || channelNode->type != ChannelNode::Type::VoiceChannel)
        return;

    if (channelNode->voiceParticipantCount == count)
        return;

    channelNode->voiceParticipantCount = count;

    QModelIndex idx = indexForNode(channelNode);
    if (idx.isValid())
        emit dataChanged(idx, idx, { VoiceParticipantCountRole });
}

void ChannelTreeModel::updateVoiceParticipant(Snowflake channelId, Snowflake userId, bool joined,
                                              Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channelId, accNode);
    if (!channelNode || channelNode->type != ChannelNode::Type::VoiceChannel)
        return;

    int existingRow = -1;
    for (size_t i = 0; i < channelNode->children.size(); ++i) {
        if (channelNode->children[i]->type == ChannelNode::Type::VoiceParticipant &&
            channelNode->children[i]->id == userId) {
            existingRow = static_cast<int>(i);
            break;
        }
    }

    QModelIndex channelIndex = indexForNode(channelNode);

    if (joined) {
        if (existingRow != -1)
            return;

        auto *instance = session->client(accountId);
        if (!instance)
            return;

        ChannelNode *guildNode = findGuildNode(channelNode);
        Snowflake guildId = guildNode ? guildNode->id : Snowflake::Invalid;

        QString displayName;
        QString avatarHash;
        auto user = instance->users()->getUser(userId);
        if (user) {
            displayName = instance->users()->getDisplayName(userId, guildId);
            if (user->avatar.hasValue())
                avatarHash = user->avatar.get();
        }
        if (displayName.isEmpty())
            displayName = QString::number(static_cast<quint64>(userId));

        int insertRow = 0;
        for (size_t i = 0; i < channelNode->children.size(); ++i) {
            const auto &child = channelNode->children[i];
            if (child->type != ChannelNode::Type::VoiceParticipant)
                continue;
            if (QString::compare(child->name, displayName, Qt::CaseInsensitive) < 0)
                insertRow = static_cast<int>(i) + 1;
            else
                break;
        }

        auto participantNode = std::make_unique<ChannelNode>();
        participantNode->id = userId;
        participantNode->parentId = channelId;
        participantNode->type = ChannelNode::Type::VoiceParticipant;
        participantNode->name = displayName;
        participantNode->dmRecipientId = userId;
        participantNode->dmAvatarHash = avatarHash;
        participantNode->parent = channelNode;

        beginInsertRows(channelIndex, insertRow, insertRow);
        channelNode->children.insert(channelNode->children.begin() + insertRow,
                                     std::move(participantNode));
        endInsertRows();
    } else {
        if (existingRow == -1)
            return;
        beginRemoveRows(channelIndex, existingRow, existingRow);
        channelNode->children.erase(channelNode->children.begin() + existingRow);
        endRemoveRows();
    }
}

void ChannelTreeModel::updateVoiceParticipantState(Snowflake channelId, Snowflake userId,
                                                   Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *channelNode = findChannelTreeNode(channelId, accNode);
    if (!channelNode || channelNode->type != ChannelNode::Type::VoiceChannel)
        return;

    for (auto &child : channelNode->children) {
        if (child->type == ChannelNode::Type::VoiceParticipant && child->id == userId) {
            QModelIndex idx = indexForNode(child.get());
            if (idx.isValid())
                emit dataChanged(idx, idx, { IsVoiceMutedRole, IsVoiceDeafenedRole });
            return;
        }
    }
}

void ChannelTreeModel::collectMarkableChannels(ChannelNode *node,
                                               QList<QPair<Snowflake, Snowflake>> &out)
{
    if (node->type == ChannelNode::Type::Channel ||
        node->type == ChannelNode::Type::DMChannel) {
        if (node->lastMessageId.isValid())
            out.append({ node->id, node->lastMessageId });
        return;
    }
    for (auto &child : node->children)
        collectMarkableChannels(child.get(), out);
}

QList<QPair<Snowflake, Snowflake>> ChannelTreeModel::getMarkableChannels(const QModelIndex &index)
{
    QList<QPair<Snowflake, Snowflake>> result;
    ChannelNode *node = nodeFromIndex(index);
    if (node)
        collectMarkableChannels(node, result);
    return result;
}

} // namespace UI
} // namespace Acheron
