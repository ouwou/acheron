#include "ChannelTreeModel.hpp"

#include <functional>

#include <Core/ClientInstance.hpp>
#include <Core/ForumManager.hpp>
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

static bool isContainerType(ChannelNode::Type type)
{
    return type == ChannelNode::Type::Category ||
           type == ChannelNode::Type::Server ||
           type == ChannelNode::Type::Folder ||
           type == ChannelNode::Type::DMHeader ||
           type == ChannelNode::Type::Channel;
}

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

static std::optional<ChannelNode::Type> nodeTypeForChannel(Discord::ChannelType type)
{
    switch (type) {
    case Discord::ChannelType::GUILD_TEXT:
    case Discord::ChannelType::GUILD_NEWS:
        return ChannelNode::Type::Channel;
    case Discord::ChannelType::GUILD_VOICE:
    case Discord::ChannelType::GUILD_STAGE_VOICE:
        return ChannelNode::Type::VoiceChannel;
    case Discord::ChannelType::GUILD_FORUM:
        return ChannelNode::Type::Forum;
    case Discord::ChannelType::GUILD_CATEGORY:
        return ChannelNode::Type::Category;
    default:
        return std::nullopt;
    }
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
                avatarTracker.notify(url, [this](const QModelIndex &index) {
                    if (index.isValid())
                        emit dataChanged(index, index, { Qt::DecorationRole });
                });
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
            QUrl iconUrl = Discord::Cdn::guildIcon(node->id, node->TEMP_iconHash, desiredSize.width());
            return avatarTracker.fetch(session->getImageManager(), iconUrl, desiredSize, index, Core::PinGroup::ChannelList);
        }

        if (node->type == ChannelNode::Type::DMChannel) {
            const QSize desiredSize(64, 64);
            QUrl avatarUrl;

            if (node->dmRecipientId.isValid()) {
                avatarUrl = Discord::Cdn::userAvatar(node->dmRecipientId, node->dmAvatarHash,
                                                     desiredSize.width());
            } else if (!node->TEMP_iconHash.isEmpty()) {
                avatarUrl = Discord::Cdn::channelIcon(node->id, node->TEMP_iconHash,
                                                      desiredSize.width());
            }

            if (!avatarUrl.isEmpty())
                return avatarTracker.fetch(session->getImageManager(), avatarUrl, desiredSize, index, Core::PinGroup::ChannelList);
        }

        if (node->type == ChannelNode::Type::VoiceParticipant &&
            node->dmRecipientId.isValid()) {
            const QSize desiredSize(32, 32);
            QUrl avatarUrl = Discord::Cdn::userAvatar(node->dmRecipientId, node->dmAvatarHash,
                                                      desiredSize.width());
            return avatarTracker.fetch(session->getImageManager(), avatarUrl, desiredSize, index, Core::PinGroup::ChannelList);
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
    if (role == CountsForGuildUnreadRole)
        return node->countsForGuildUnread;
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
    if (role == IconHashRole)
        return node->TEMP_iconHash;
    if (role == FolderColorRole) {
        if (node->folderColor.has_value())
            return static_cast<quint64>(node->folderColor.value());
        return {};
    }
    if (role == OwnerIdRole)
        return static_cast<quint64>(node->ownerId);

    if (role == ThreadJoinedRole) {
        if (node->type != ChannelNode::Type::Thread ||
            !node->parent ||
            node->parent->type != ChannelNode::Type::Channel)
            return false;
        ChannelNode *accNode = getAccountNodeFor(node);
        if (!accNode)
            return false;
        auto *instance = session->client(accNode->id);
        return instance && instance->isThreadJoined(node->id);
    }

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

    if (node->opensChat())
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

        const auto &guilds = ready.guilds.get();
        for (auto it = guilds.rbegin(); it != guilds.rend(); ++it)
            if (!guildIdsInFolders.contains(it->properties->id))
                topLevelNodes.push_back(createGuildNode(*it, instance));

        for (const auto &folder : folders) {
            if (!folder.id.has_value()) {
                // folders with null ids are just guilds
                for (const auto &guildId : folder.guildIds)
                    if (guildMap.contains(guildId))
                        topLevelNodes.push_back(createGuildNode(*guildMap[guildId], instance));
            } else {
                auto folderNode = createFolderNode(folder);

                for (const auto &guildId : folder.guildIds)
                    if (guildMap.contains(guildId))
                        folderNode->addChild(createGuildNode(*guildMap[guildId], instance));

                if (!folderNode->children.empty())
                    topLevelNodes.push_back(std::move(folderNode));
            }
        }
    } else if (settings.guildFolders.has_value() &&
               !settings.guildFolders->guildPositions.isEmpty()) {
        const auto &positions = settings.guildFolders->guildPositions;
        QSet<Core::Snowflake> positioned;
        for (const auto &id : positions)
            positioned.insert(id);

        const auto &guilds = ready.guilds.get();
        for (auto it = guilds.rbegin(); it != guilds.rend(); ++it)
            if (!positioned.contains(it->properties->id))
                topLevelNodes.push_back(createGuildNode(*it, instance));

        for (const auto &guildId : positions)
            if (guildMap.contains(guildId))
                topLevelNodes.push_back(createGuildNode(*guildMap[guildId], instance));
    } else {
        const auto &guilds = ready.guilds.get();
        for (auto it = guilds.rbegin(); it != guilds.rend(); ++it)
            topLevelNodes.push_back(createGuildNode(*it, instance));
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

std::unique_ptr<ChannelNode> ChannelTreeModel::createGuildNode(const Discord::GatewayGuild &guild, Core::ClientInstance *instance)
{
    auto guildNode = std::make_unique<ChannelNode>();
    guildNode->id = guild.properties->id;
    guildNode->name = guild.properties->name;
    guildNode->type = ChannelNode::Type::Server;
    guildNode->TEMP_iconHash = guild.properties->icon;
    guildNode->ownerId = guild.properties->ownerId;
    guildNode->unavailable = guild.unavailable.hasValue() && guild.unavailable.get();
    if (guild.properties->rulesChannelId.hasValue() && guild.properties->rulesChannelId->isValid())
        guildNode->rulesChannelId = guild.properties->rulesChannelId.get();

    QHash<Snowflake, ChannelNode *> categoryMap;
    QHash<Snowflake, ChannelNode *> textChannelMap;
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
        auto nodeType = nodeTypeForChannel(channel.type);
        if (!nodeType || *nodeType == ChannelNode::Type::Category)
            continue;

        auto node = std::make_unique<ChannelNode>();
        node->id = channel.id;
        node->name = channel.name;
        node->type = *nodeType;
        node->position = channel.position;
        node->parentId = channel.parentId.hasValue() ? channel.parentId.get() : Core::Snowflake();
        node->isPrivate = isChannelPrivate(channel, guild.properties->id);
        if (*nodeType == ChannelNode::Type::VoiceChannel) {
            if (channel.userLimit.hasValue())
                node->userLimit = channel.userLimit.get();
        } else {
            node->lastMessageId = channel.lastMessageId.hasValue() ? channel.lastMessageId.get()
                                                                   : Core::Snowflake();
        }

        ChannelNode *rawNode = node.get();
        bool placed = false;
        if (channel.parentId.hasValue() && channel.parentId->isValid()) {
            if (categoryMap.contains(channel.parentId.get())) {
                categoryMap[channel.parentId.get()]->addChild(std::move(node));
                placed = true;
            }
        } else {
            orphanChannels.push_back(std::move(node));
            placed = true;
        }
        if (placed && *nodeType == ChannelNode::Type::Channel)
            textChannelMap.insert(channel.id.get(), rawNode);
    }

    if (guild.threads.hasValue()) {
        for (const auto &thread : guild.threads.get()) {
            if (thread.isArchived() || !thread.parentId.hasValue())
                continue;
            if (instance && !instance->readState()->isThreadRelevant(thread))
                continue;
            auto it = textChannelMap.constFind(thread.parentId.get());
            if (it != textChannelMap.constEnd())
                it.value()->addChild(makeThreadNode(thread));
        }
        for (auto it = textChannelMap.begin(); it != textChannelMap.end(); ++it)
            std::sort(it.value()->children.begin(), it.value()->children.end(),
                      [](const auto &a, const auto &b) { return a->lastMessageId > b->lastMessageId; });
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

std::unique_ptr<ChannelNode> ChannelTreeModel::makeThreadNode(const Discord::Channel &thread)
{
    auto node = std::make_unique<ChannelNode>();
    node->id = thread.id;
    node->name = thread.name.hasValue() ? thread.name.get() : QString();
    node->type = ChannelNode::Type::Thread;
    node->parentId = thread.parentId.hasValue() ? thread.parentId.get() : Core::Snowflake();
    node->isArchived = thread.isArchived();
    node->isPrivate = thread.type.hasValue() && thread.type.get() == Discord::ChannelType::PRIVATE_THREAD;
    node->lastMessageId = thread.effectiveLastMessageId();
    return node;
}

ChannelNode *ChannelTreeModel::insertThreadNode(const Discord::Channel &thread, Snowflake accountId, bool temporary)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode || !thread.parentId.hasValue())
        return nullptr;

    if (ChannelNode *existing = findChannelTreeNode(thread.id.get(), accNode))
        return existing;

    ChannelNode *parentChannel = findChannelTreeNode(thread.parentId.get(), accNode);
    if (!parentChannel || parentChannel->type != ChannelNode::Type::Channel)
        return nullptr;

    auto node = makeThreadNode(thread);
    node->isTemporary = temporary;
    ChannelNode *raw = node.get();

    int row = 0;
    for (; row < static_cast<int>(parentChannel->children.size()); ++row)
        if (parentChannel->children[row]->lastMessageId < node->lastMessageId)
            break;

    insertChildAt(parentChannel, row, std::move(node));

    if (auto *instance = session->client(accountId)) {
        ChannelNode *guildNode = findGuildNode(parentChannel);
        Snowflake guildId = guildNode ? guildNode->id : Snowflake::Invalid;
        applyChannelReadState(raw, computeNodeReadState(raw, guildId, instance));
        updateNodeAggregates(parentChannel);
    }

    QModelIndex parentIdx = indexForNode(parentChannel);
    if (parentIdx.isValid())
        emit dataChanged(parentIdx, parentIdx);

    return raw;
}

void ChannelTreeModel::addThread(const Discord::Channel &thread, Snowflake accountId)
{
    if (thread.isArchived())
        return;

    auto *instance = session->client(accountId);
    if (!instance || !instance->isThreadJoined(thread.id.get()) ||
        !instance->readState()->isThreadRelevant(thread))
        return;
    insertThreadNode(thread, accountId);
}

void ChannelTreeModel::updateThread(const Discord::Channel &thread, Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *node = findChannelTreeNode(thread.id.get(), accNode);

    if (thread.isArchived()) {
        if (!node)
            return;
        if (!node->isTemporary) {
            removeThread(thread.id.get(), accountId);
            return;
        }
        node->isArchived = true;
    } else if (!node) {
        addThread(thread, accountId);
        return;
    } else {
        node->isArchived = false;
    }

    if (thread.name.hasValue())
        node->name = thread.name.get();
    if (thread.lastMessageId.hasValue())
        node->lastMessageId = thread.effectiveLastMessageId();

    QModelIndex idx = indexForNode(node);
    if (idx.isValid())
        emit dataChanged(idx, idx, { Qt::DisplayRole, LastMessageIdRole });

    resortThread(node);
}

void ChannelTreeModel::removeThread(Snowflake threadId, Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *node = findChannelTreeNode(threadId, accNode);
    if (!node || node->type != ChannelNode::Type::Thread)
        return;

    ChannelNode *parent = node->parent;
    if (!parent || parent->type != ChannelNode::Type::Channel)
        return;

    if (!removeChildRow(parent, node))
        return;

    updateNodeAggregates(parent);
    QModelIndex parentIdx = indexForNode(parent);
    if (parentIdx.isValid())
        emit dataChanged(parentIdx, parentIdx);
}

bool ChannelTreeModel::removeChildRow(ChannelNode *parent, ChannelNode *node)
{
    for (size_t i = 0; i < parent->children.size(); ++i) {
        if (parent->children[i].get() != node)
            continue;
        int row = static_cast<int>(i);
        beginRemoveRows(indexForNode(parent), row, row);
        parent->children.erase(parent->children.begin() + row);
        endRemoveRows();
        return true;
    }
    return false;
}

void ChannelTreeModel::resortThread(ChannelNode *node)
{
    ChannelNode *parent = node->parent;
    if (!parent || parent->type != ChannelNode::Type::Channel)
        return;

    auto &kids = parent->children;
    int row = -1;
    for (size_t i = 0; i < kids.size(); ++i)
        if (kids[i].get() == node) {
            row = static_cast<int>(i);
            break;
        }
    if (row == -1)
        return;

    int dest = static_cast<int>(kids.size());
    for (size_t i = 0; i < kids.size(); ++i) {
        if (static_cast<int>(i) == row)
            continue;
        if (kids[i]->lastMessageId < node->lastMessageId) {
            dest = static_cast<int>(i);
            break;
        }
    }
    if (dest == row || dest == row + 1)
        return;

    QModelIndex parentIdx = indexForNode(parent);
    if (!beginMoveRows(parentIdx, row, row, parentIdx, dest))
        return;
    auto moved = std::move(kids[row]);
    kids.erase(kids.begin() + row);
    kids.insert(kids.begin() + (dest > row ? dest - 1 : dest), std::move(moved));
    endMoveRows();
}

void ChannelTreeModel::syncThreads(Snowflake guildId, const QList<Snowflake> &parentIds,
                                   const QList<Discord::Channel> &threads, Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;
    ChannelNode *guildNode = findGuildNodeById(guildId, accNode);
    if (!guildNode)
        return;

    auto *instance = session->client(accountId);

    for (const auto &thread : threads)
        updateThread(thread, accountId);

    QSet<Snowflake> parentFilter(parentIds.begin(), parentIds.end());
    QList<Snowflake> toRemove;
    std::function<void(ChannelNode *)> collect = [&](ChannelNode *node) {
        for (const auto &child : node->children) {
            if (child->type == ChannelNode::Type::Thread) {
                if (node->type != ChannelNode::Type::Channel)
                    continue;
                bool inScope = parentFilter.isEmpty() || parentFilter.contains(child->parentId);
                if (!inScope || child->isTemporary)
                    continue;
                bool keep = false;
                if (instance && instance->isThreadJoined(child->id)) {
                    auto ch = instance->getChannel(child->id);
                    keep = ch && instance->readState()->isThreadRelevant(*ch);
                }
                if (!keep)
                    toRemove.append(child->id);
            } else {
                collect(child.get());
            }
        }
    };
    collect(guildNode);

    for (Snowflake id : toRemove)
        removeThread(id, accountId);
}

void ChannelTreeModel::showTemporaryThread(const Discord::Channel &thread, Snowflake accountId)
{
    Snowflake threadId = thread.id.get();
    if (temporaryThreadId == threadId && temporaryThreadAccount == accountId)
        return;

    clearTemporaryThread();

    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *node = findChannelTreeNode(threadId, accNode);
    if (!node) {
        node = insertThreadNode(thread, accountId, true);
        if (!node)
            return;
    } else {
        node->isTemporary = true;
        QModelIndex idx = indexForNode(node);
        if (idx.isValid())
            emit dataChanged(idx, idx);
    }

    temporaryThreadId = threadId;
    temporaryThreadAccount = accountId;
}

void ChannelTreeModel::clearTemporaryThread(Snowflake exceptThreadId)
{
    if (!temporaryThreadId.isValid() || temporaryThreadId == exceptThreadId)
        return;

    Snowflake id = temporaryThreadId;
    Snowflake acc = temporaryThreadAccount;
    temporaryThreadId = Snowflake::Invalid;
    temporaryThreadAccount = Snowflake::Invalid;

    ChannelNode *accNode = accountNodes.value(acc, nullptr);
    if (!accNode)
        return;
    ChannelNode *node = findChannelTreeNode(id, accNode);
    if (node && node->type == ChannelNode::Type::Thread && node->isTemporary)
        removeThread(id, acc);
}

void ChannelTreeModel::promoteTemporaryThread(Snowflake threadId)
{
    if (temporaryThreadId == threadId) {
        temporaryThreadId = Snowflake::Invalid;
        temporaryThreadAccount = Snowflake::Invalid;
    }

    for (auto it = accountNodes.begin(); it != accountNodes.end(); ++it) {
        if (ChannelNode *node = findChannelTreeNode(threadId, it.value())) {
            node->isTemporary = false;
            return;
        }
    }
}

std::unique_ptr<ChannelNode> ChannelTreeModel::createFolderNode(const Proto::GuildFolder &folder)
{
    auto folderNode = std::make_unique<ChannelNode>();
    folderNode->type = ChannelNode::Type::Folder;
    folderNode->name = folder.name.value_or("Unnamed Folder");
    folderNode->folderName = folder.name;
    if (folder.color != Discord::BLURPLE)
        folderNode->folderColor = folder.color;
    folderNode->id = Core::Snowflake(folder.id.value());
    return folderNode;
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

QModelIndex ChannelTreeModel::serverIndex(Snowflake accountId, Snowflake guildId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return {};
    ChannelNode *guildNode = findGuildNodeById(guildId, accNode);
    return guildNode ? indexForNode(guildNode) : QModelIndex();
}

QModelIndex ChannelTreeModel::folderIndex(Snowflake accountId, Snowflake folderId)
{
    ChannelNode *folderNode = findFolderNodeById(accountNodes.value(accountId, nullptr), folderId);
    return folderNode ? indexForNode(folderNode) : QModelIndex();
}

QModelIndex ChannelTreeModel::dmHeaderIndex(Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return {};
    for (const auto &child : accNode->children)
        if (child->type == ChannelNode::Type::DMHeader)
            return indexForNode(child.get());
    return {};
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

ChannelNode *ChannelTreeModel::findFolderNodeById(ChannelNode *accountNode, Snowflake folderId)
{
    if (!accountNode)
        return nullptr;

    for (const auto &child : accountNode->children)
        if (child->type == ChannelNode::Type::Folder && child->id == folderId)
            return child.get();

    return nullptr;
}

void ChannelTreeModel::insertChildAt(ChannelNode *parent, int row, std::unique_ptr<ChannelNode> node)
{
    QModelIndex parentIdx = indexForNode(parent);
    beginInsertRows(parentIdx, row, row);
    node->parent = parent;
    parent->children.insert(parent->children.begin() + row, std::move(node));
    endInsertRows();
}

void ChannelTreeModel::addGuild(const Discord::GatewayGuild &guild, Snowflake accountId)
{
    if (!guild.properties.hasValue())
        return;

    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    auto *instance = session->client(accountId);
    Snowflake guildId = guild.properties->id.get();

    auto guildNode = createGuildNode(guild, instance);
    ChannelNode *guildPtr = guildNode.get();

    // compute read state on the detached subtree so the views see final values on insert
    if (instance) {
        initChannelReadStates(guildPtr, instance);
        recomputeSubtreeAggregates(guildPtr);
    }

    // handle a (potentially) unavailable guild coming back
    if (ChannelNode *existing = findGuildNodeById(guildId, accNode)) {
        QModelIndex existingIdx = indexForNode(existing);

        if (!existing->unavailable) {
            existing->name = guildPtr->name;
            existing->TEMP_iconHash = guildPtr->TEMP_iconHash;
            if (guildPtr->rulesChannelId.isValid())
                existing->rulesChannelId = guildPtr->rulesChannelId;
            if (existingIdx.isValid())
                emit dataChanged(existingIdx, existingIdx);
            return;
        }

        ChannelNode *parentNode = existing->parent;
        if (!parentNode || !existingIdx.isValid())
            return;

        int existingRow = existingIdx.row();

        if (!removeChildRow(parentNode, existing))
            return;

        insertChildAt(parentNode, existingRow, std::move(guildNode));

        if (parentNode->type == ChannelNode::Type::Folder)
            updateNodeAggregates(parentNode);
        return;
    }

    placeGuildNode(accNode, guildId, std::move(guildNode), instance);
}

void ChannelTreeModel::removeGuild(Snowflake accountId, Snowflake guildId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *guildNode = findGuildNodeById(guildId, accNode);
    if (!guildNode)
        return;

    ChannelNode *parent = guildNode->parent;
    if (!parent)
        return;

    removeChildRow(parent, guildNode);
}

void ChannelTreeModel::placeGuildNode(ChannelNode *accNode, Snowflake guildId,
                                      std::unique_ptr<ChannelNode> guildNode,
                                      Core::ClientInstance *instance)
{
    auto topRow = [accNode] {
        int r = 0;
        while (r < static_cast<int>(accNode->children.size()) &&
               accNode->children[r]->type == ChannelNode::Type::DMHeader)
            r++;
        return r;
    };

    const Proto::GuildFolder *folder = nullptr;
    if (instance) {
        const auto &settings = instance->discord()->getSettings();
        if (settings.guildFolders.has_value())
            for (const auto &f : settings.guildFolders->folders)
                if (f.id.has_value() && f.guildIds.contains(guildId)) {
                    folder = &f;
                    break;
                }
    }

    ChannelNode *parent = accNode;
    int row = topRow();

    if (folder) {
        if (ChannelNode *folderNode = findFolderNodeById(accNode, Core::Snowflake(folder->id.value()))) {
            parent = folderNode;
            row = 0;
            for (Snowflake gid : folder->guildIds) {
                if (gid == guildId)
                    break;
                for (const auto &c : folderNode->children)
                    if (c->type == ChannelNode::Type::Server && c->id == gid) {
                        row++;
                        break;
                    }
            }
        } else {
            auto newFolder = createFolderNode(*folder);
            newFolder->addChild(std::move(guildNode));
            recomputeSubtreeAggregates(newFolder.get());
            guildNode = std::move(newFolder);
        }
    }

    insertChildAt(parent, row, std::move(guildNode));

    if (parent->type == ChannelNode::Type::Folder)
        updateNodeAggregates(parent);
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

        insertChildAt(dmHeader, 0, std::move(dmNode));

        return;
    }

    auto nodeType = nodeTypeForChannel(channel.type);
    if (!nodeType)
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

    node->type = *nodeType;
    if (*nodeType == ChannelNode::Type::Category) {
        insertChildAt(guildNode, static_cast<int>(guildNode->children.size()), std::move(node));
    } else {
        node->isPrivate = isChannelPrivate(channel, guildId);
        if (*nodeType == ChannelNode::Type::VoiceChannel && channel.userLimit.hasValue())
            node->userLimit = channel.userLimit.get();

        ChannelNode *parentNode = nullptr;
        if (node->parentId.isValid())
            parentNode = findCategoryNode(node->parentId, guildNode);
        if (!parentNode)
            parentNode = guildNode;

        insertChildAt(parentNode, static_cast<int>(parentNode->children.size()), std::move(node));

        // notify proxy to re-check category visibility
        if (parentNode->type == ChannelNode::Type::Category) {
            QModelIndex parentIdx = indexForNode(parentNode);
            if (parentIdx.isValid())
                emit dataChanged(parentIdx, parentIdx);
        }
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

    if (!removeChildRow(parent, channelNode))
        return;

    // notify proxy to re-check category visibility
    QModelIndex parentIdx = indexForNode(parent);
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

static Core::ChannelReadState forumPostReadState(Core::ReadStateManager *readState, const ChannelNode *node, Snowflake guildId)
{
    Core::ChannelReadState state;
    state.isUnread = readState->isForumPostUnread(node->id, node->lastMessageId, false);
    state.mentionCount = readState->getMentionCount(node->id);
    state.isMuted = readState->isChannelMuted(node->id);
    bool guildMuted = guildId.isValid() && readState->isGuildMuted(guildId);
    state.countsForGuildUnread = state.mentionCount > 0 || (state.isUnread && !state.isMuted && !guildMuted);
    return state;
}

void ChannelTreeModel::applyForumReadState(ChannelNode *node, Core::ReadStateManager *readState, Snowflake guildId)
{
    applyChannelReadState(node, readState->computeChannelReadState(node->id, guildId, node->parentId, false));
    for (const auto &post : node->children) {
        node->subtreeMentionCount += post->subtreeMentionCount;
        if (post->countsForGuildUnread)
            node->countsForGuildUnread = true;
    }
}

bool ChannelTreeModel::refreshForumNode(ChannelNode *forumNode, Core::ClientInstance *instance,
                                        Snowflake guildId)
{
    ReadStateSnapshot before = readStateSnapshot(forumNode);
    applyForumReadState(forumNode, instance->readState(), guildId);
    return notifyIfReadStateChanged(forumNode, before);
}

ChannelTreeModel::ReadStateSnapshot ChannelTreeModel::readStateSnapshot(const ChannelNode *node)
{
    return { node->isUnread, node->isMuted, node->countsForGuildUnread, node->mentionCount, node->subtreeMentionCount };
}

bool ChannelTreeModel::notifyIfReadStateChanged(ChannelNode *node, const ReadStateSnapshot &before)
{
    if (before.isUnread == node->isUnread && before.isMuted == node->isMuted &&
        before.countsForGuildUnread == node->countsForGuildUnread &&
        before.mentionCount == node->mentionCount &&
        before.subtreeMentionCount == node->subtreeMentionCount)
        return false;

    QModelIndex idx = indexForNode(node);
    if (idx.isValid())
        emit dataChanged(idx, idx, { IsUnreadRole, MentionCountRole, IsMutedRole });
    return true;
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

    ReadStateSnapshot before = readStateSnapshot(channelNode);

    if (channelNode->type == ChannelNode::Type::Forum) {
        applyForumReadState(channelNode, instance->readState(), guildId);
    } else {
        applyChannelReadState(channelNode, computeNodeReadState(channelNode, guildId, instance));
        if (isContainerType(channelNode->type))
            aggregateChildren(channelNode);
    }

    if (!notifyIfReadStateChanged(channelNode, before))
        return;

    ChannelNode *parent = channelNode->parent;
    if (parent && parent->type == ChannelNode::Type::Forum) {
        if (!refreshForumNode(parent, instance, guildId))
            return;
        parent = parent->parent;
    }
    if (parent)
        updateNodeAggregates(parent);
}

void ChannelTreeModel::updateForumBadge(Snowflake forumId, Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *node = findChannelTreeNode(forumId, accNode);
    if (!node || node->type != ChannelNode::Type::Forum)
        return;

    auto *instance = session->client(accountId);
    if (!instance)
        return;

    Core::ForumBadge badge = instance->forums()->badge(forumId);
    if (badge.count == node->forumBadgeCount && badge.isNew == node->forumBadgeIsNew)
        return;

    node->forumBadgeCount = badge.count;
    node->forumBadgeIsNew = badge.isNew;
    QModelIndex idx = indexForNode(node);
    if (idx.isValid())
        emit dataChanged(idx, idx);
}

void ChannelTreeModel::updateForumThreads(Snowflake forumId, Snowflake accountId)
{
    ChannelNode *accNode = accountNodes.value(accountId, nullptr);
    if (!accNode)
        return;

    ChannelNode *forumNode = findChannelTreeNode(forumId, accNode);
    if (!forumNode || forumNode->type != ChannelNode::Type::Forum)
        return;

    auto *instance = session->client(accountId);
    if (!instance)
        return;

    const QList<Discord::Channel> posts = instance->forums()->joinedPosts(forumId);

    if (posts.size() == static_cast<int>(forumNode->children.size())) {
        bool same = true;
        for (int i = 0; i < posts.size() && same; i++)
            same = forumNode->children[i]->id == posts[i].id.get();
        if (same) {
            for (int i = 0; i < posts.size(); i++) {
                ChannelNode *child = forumNode->children[i].get();
                child->lastMessageId = posts[i].effectiveLastMessageId();
                const QString name = posts[i].name.hasValue() ? posts[i].name.get() : QString();
                if (child->name == name)
                    continue;
                child->name = name;
                QModelIndex idx = indexForNode(child);
                if (idx.isValid())
                    emit dataChanged(idx, idx);
            }
            return;
        }
    }

    QModelIndex forumIdx = indexForNode(forumNode);
    if (!forumIdx.isValid())
        return;

    if (!forumNode->children.empty()) {
        beginRemoveRows(forumIdx, 0, static_cast<int>(forumNode->children.size()) - 1);
        forumNode->children.clear();
        endRemoveRows();
    }

    ChannelNode *guildNode = findGuildNode(forumNode);
    Snowflake guildId = guildNode ? guildNode->id : Snowflake::Invalid;

    if (!posts.isEmpty()) {
        beginInsertRows(forumIdx, 0, posts.size() - 1);
        for (const auto &post : posts) {
            auto node = std::make_unique<ChannelNode>();
            node->id = post.id.get();
            node->name = post.name.hasValue() ? post.name.get() : QString();
            node->type = ChannelNode::Type::Thread;
            node->parentId = forumId;
            node->lastMessageId = post.effectiveLastMessageId();
            ChannelNode *added = forumNode->addChild(std::move(node));
            applyChannelReadState(added, forumPostReadState(instance->readState(), added, guildId));
        }
        endInsertRows();
    }

    if (refreshForumNode(forumNode, instance, guildId) && forumNode->parent)
        updateNodeAggregates(forumNode->parent);
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
    recomputeSubtreeAggregates(targetNode);
    QModelIndex targetIdx = indexForNode(targetNode);
    if (targetIdx.isValid())
        emitDataChangedRecursive(targetIdx);
    if (targetNode->parent)
        updateNodeAggregates(targetNode->parent);
}

void ChannelTreeModel::applyChannelReadState(ChannelNode *node, const Core::ChannelReadState &state)
{
    node->selfUnread = state.isUnread;
    node->selfMentionCount = state.mentionCount;
    node->selfCountsForGuildUnread = state.countsForGuildUnread;
    node->isMuted = state.isMuted;

    node->isUnread = state.isUnread;
    node->mentionCount = state.mentionCount;
    node->subtreeMentionCount = state.mentionCount;
    node->countsForGuildUnread = state.countsForGuildUnread;
}

Core::ChannelReadState ChannelTreeModel::computeNodeReadState(ChannelNode *node, Snowflake guildId, Core::ClientInstance *instance)
{
    if (node->type == ChannelNode::Type::Thread) {
        if (node->parent && node->parent->type == ChannelNode::Type::Forum)
            return forumPostReadState(instance->readState(), node, guildId);

        bool joined = instance->isThreadJoined(node->id);
        auto state = instance->readState()->computeThreadReadState(node->id, guildId, node->parentId, joined);
        if (node->parent && node->parent->type == ChannelNode::Type::Channel) {
            bool parentMuted = node->parent->isMuted ||
                               (node->parent->parentId.isValid() &&
                                instance->readState()->isChannelMuted(node->parent->parentId));
            if (parentMuted) {
                state.isMuted = true;
                state.countsForGuildUnread = false;
            }
        }
        return state;
    }

    bool isDM = node->type == ChannelNode::Type::DMChannel;
    return instance->readState()->computeChannelReadState(node->id, guildId, node->parentId, isDM);
}

void ChannelTreeModel::aggregateChildren(ChannelNode *node)
{
    bool isChannel = node->type == ChannelNode::Type::Channel;
    node->mentionCount = isChannel ? node->selfMentionCount : 0;
    node->isUnread = isChannel ? node->selfUnread : false;
    node->countsForGuildUnread = isChannel ? node->selfCountsForGuildUnread : false;
    for (const auto &child : node->children) {
        if (child->isUnread && !child->isMuted)
            node->isUnread = true;

        if (child->countsForGuildUnread)
            node->countsForGuildUnread = true;

        node->mentionCount += child->subtreeMentionCount;
    }
    node->subtreeMentionCount = node->mentionCount;
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
    bool oldCounts = node->countsForGuildUnread;

    aggregateChildren(node);

    if (oldMentionCount != node->mentionCount || oldIsUnread != node->isUnread || oldCounts != node->countsForGuildUnread) {
        QModelIndex idx = indexForNode(node);
        if (idx.isValid())
            emit dataChanged(idx, idx, { IsUnreadRole, CountsForGuildUnreadRole, MentionCountRole });

        if (node->parent)
            updateNodeAggregates(node->parent);
    }
}

void ChannelTreeModel::initChannelReadStates(ChannelNode *node, Core::ClientInstance *instance)
{
    if (node->type == ChannelNode::Type::Server)
        node->isMuted = instance->readState()->isGuildMuted(node->id);
    else if (node->type == ChannelNode::Type::Category)
        node->isMuted = instance->readState()->isChannelMuted(node->id);

    if (node->type == ChannelNode::Type::Forum) {
        ChannelNode *guildNode = findGuildNode(node);
        Snowflake guildId = guildNode ? guildNode->id : Snowflake::Invalid;
        applyForumReadState(node, instance->readState(), guildId);
    } else if (node->type == ChannelNode::Type::Channel ||
               node->type == ChannelNode::Type::DMChannel ||
               node->type == ChannelNode::Type::Thread) {
        ChannelNode *guildNode = findGuildNode(node);
        Snowflake guildId = guildNode ? guildNode->id : Snowflake::Invalid;
        applyChannelReadState(node, computeNodeReadState(node, guildId, instance));
    }

    for (const auto &child : node->children)
        initChannelReadStates(child.get(), instance);
}

void ChannelTreeModel::updateChildrenReadState(ChannelNode *node, Snowflake guildId,
                                               Core::ClientInstance *instance)
{
    for (const auto &child : node->children) {
        if (child->type == ChannelNode::Type::Channel ||
            child->type == ChannelNode::Type::DMChannel ||
            child->type == ChannelNode::Type::Thread) {
            ReadStateSnapshot before = readStateSnapshot(child.get());
            applyChannelReadState(child.get(), computeNodeReadState(child.get(), guildId, instance));
            notifyIfReadStateChanged(child.get(), before);
        } else if (child->type == ChannelNode::Type::Category) {
            child->isMuted = instance->readState()->isChannelMuted(child->id);

            QModelIndex idx = indexForNode(child.get());
            if (idx.isValid())
                emit dataChanged(idx, idx, { IsMutedRole });
        } else if (child->type == ChannelNode::Type::Forum) {
            for (const auto &post : child->children) {
                ReadStateSnapshot before = readStateSnapshot(post.get());
                applyChannelReadState(post.get(), forumPostReadState(instance->readState(), post.get(), guildId));
                notifyIfReadStateChanged(post.get(), before);
            }
            refreshForumNode(child.get(), instance, guildId);
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
