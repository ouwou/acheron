#pragma once

#include <QtWidgets>

#include "ChannelNode.hpp"
#include "Core/AccountInfo.hpp"

#include "Core/Snowflake.hpp"
#include "Core/Session.hpp"
#include "Discord/Events.hpp"
#include "UI/AvatarRequestTracker.hpp"

using Acheron::Core::Session;
using Acheron::Core::Snowflake;

namespace Acheron::Core {
struct ChannelReadState;
class ClientInstance;
class ReadStateManager;
} // namespace Acheron::Core

namespace Acheron::Proto {
struct GuildFolder;
} // namespace Acheron::Proto

namespace Acheron {
namespace UI {
class ChannelTreeModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    ChannelTreeModel(Session *session, QObject *parent = nullptr);

    enum Roles {
        IdRole = Qt::UserRole,
        TypeRole = Qt::UserRole + 1,
        PositionRole = Qt::UserRole + 2,
        LastMessageIdRole = Qt::UserRole + 3,
        IsUnreadRole = Qt::UserRole + 4,
        MentionCountRole = Qt::UserRole + 5,
        IsMutedRole = Qt::UserRole + 6,
        CollapsedRole = Qt::UserRole + 7,
        VoiceParticipantCountRole = Qt::UserRole + 8,
        UserLimitRole = Qt::UserRole + 9,
        IsVoiceMutedRole = Qt::UserRole + 10,
        IsVoiceDeafenedRole = Qt::UserRole + 11,
        IconHashRole = Qt::UserRole + 12,
        FolderColorRole = Qt::UserRole + 13,
        CountsForGuildUnreadRole = Qt::UserRole + 14,
        ThreadJoinedRole = Qt::UserRole + 15,
        OwnerIdRole = Qt::UserRole + 16,
    };

    QModelIndex index(int row, int column, const QModelIndex &parentIndex) const override;
    QModelIndex parent(const QModelIndex &childIndex) const override;
    int rowCount(const QModelIndex &parentIndex) const override;
    int columnCount(const QModelIndex &) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    void addAccount(const Acheron::Core::AccountInfo &account);
    void removeAccount(Snowflake accountId);

    void populateFromReady(const Discord::Ready &ready);

    static ChannelNode *getAccountNodeFor(ChannelNode *node);
    static ChannelNode *findGuildNode(ChannelNode *node);

    ChannelNode *nodeFromIndex(const QModelIndex &index) const;
    void addGuild(const Discord::GatewayGuild &guild, Snowflake accountId);
    void removeGuild(Snowflake accountId, Snowflake guildId);
    void addChannel(const Discord::ChannelCreate &event, Snowflake accountId);
    void updateChannel(const Discord::ChannelUpdate &update, Snowflake accountId);
    void deleteChannel(const Discord::ChannelDelete &event, Snowflake accountId);
    void addThread(const Discord::Channel &thread, Snowflake accountId);
    void updateThread(const Discord::Channel &thread, Snowflake accountId);
    void removeThread(Snowflake threadId, Snowflake accountId);
    void syncThreads(Snowflake guildId, const QList<Snowflake> &parentIds, const QList<Discord::Channel> &threads, Snowflake accountId);
    void showTemporaryThread(const Discord::Channel &thread, Snowflake accountId);
    void clearTemporaryThread(Snowflake exceptThreadId = Snowflake::Invalid);
    void promoteTemporaryThread(Snowflake threadId);
    void invalidateGuildData(Snowflake guildId);
    void updateReadState(Snowflake channelId, Snowflake accountId);
    void updateForumBadge(Snowflake forumId, Snowflake accountId);
    void updateForumThreads(Snowflake forumId, Snowflake accountId);
    void updateGuildSettings(Snowflake guildId, Snowflake accountId);
    void updateChannelLastMessageId(Snowflake channelId, Snowflake messageId, Snowflake accountId);
    void updateVoiceCount(Snowflake channelId, int count, Snowflake accountId);
    void updateVoiceParticipant(Snowflake channelId, Snowflake userId, bool joined, Snowflake accountId);
    void updateVoiceParticipantState(Snowflake channelId, Snowflake userId, Snowflake accountId);
    void toggleCollapsed(const QModelIndex &index);
    void setCollapsed(const QModelIndex &index, bool collapsed);

    QList<QPair<Snowflake, Snowflake>> getMarkableChannels(const QModelIndex &index);
    ChannelNode *findChannelTreeNode(Snowflake channelId);
    ChannelNode *findChannelTreeNode(Snowflake channelId, Snowflake accountId);
    QModelIndex indexForNode(ChannelNode *node) const;

    QModelIndex serverIndex(Snowflake accountId, Snowflake guildId);
    QModelIndex folderIndex(Snowflake accountId, Snowflake folderId);
    QModelIndex dmHeaderIndex(Snowflake accountId);

private:
    void initChannelReadStates(ChannelNode *node, Core::ClientInstance *instance);
    void updateChildrenReadState(ChannelNode *node, Snowflake guildId,
                                 Core::ClientInstance *instance);
    static void collectMarkableChannels(ChannelNode *node,
                                        QList<QPair<Snowflake, Snowflake>> &out);
    void applyChannelReadState(ChannelNode *node, const Core::ChannelReadState &state);
    Core::ChannelReadState computeNodeReadState(ChannelNode *node, Snowflake guildId, Core::ClientInstance *instance);
    void applyForumReadState(ChannelNode *node, Core::ReadStateManager *readState, Snowflake guildId);
    struct ReadStateSnapshot
    {
        bool isUnread;
        bool isMuted;
        bool countsForGuildUnread;
        int mentionCount;
        int subtreeMentionCount;
    };
    static ReadStateSnapshot readStateSnapshot(const ChannelNode *node);
    bool notifyIfReadStateChanged(ChannelNode *node, const ReadStateSnapshot &before);
    bool refreshForumNode(ChannelNode *forumNode, Core::ClientInstance *instance, Snowflake guildId);
    static void aggregateChildren(ChannelNode *node);
    void recomputeSubtreeAggregates(ChannelNode *root);
    void updateNodeAggregates(ChannelNode *node);
    std::unique_ptr<ChannelNode> createGuildNode(const Discord::GatewayGuild &guild, Core::ClientInstance *instance);
    static std::unique_ptr<ChannelNode> makeThreadNode(const Discord::Channel &thread);
    ChannelNode *insertThreadNode(const Discord::Channel &thread, Snowflake accountId, bool temporary = false);
    bool removeChildRow(ChannelNode *parent, ChannelNode *node);
    void resortThread(ChannelNode *node);
    std::unique_ptr<ChannelNode> createFolderNode(const Proto::GuildFolder &folder);
    void placeGuildNode(ChannelNode *accNode, Snowflake guildId, std::unique_ptr<ChannelNode> guildNode, Core::ClientInstance *instance);
    ChannelNode *findChannelTreeNode(Snowflake channelId, ChannelNode *root);
    ChannelNode *findGuildNodeById(Snowflake guildId, ChannelNode *accountNode);
    ChannelNode *findFolderNodeById(ChannelNode *accountNode, Snowflake folderId);
    ChannelNode *findCategoryNode(Snowflake categoryId, ChannelNode *guildNode);
    void insertChildAt(ChannelNode *parent, int row, std::unique_ptr<ChannelNode> node);
    void emitDataChangedRecursive(const QModelIndex &index);

private:
    Session *session;

    std::unique_ptr<ChannelNode> root;
    QHash<Snowflake, ChannelNode *> accountNodes;
    mutable AvatarRequestTracker<QPersistentModelIndex> avatarTracker;

    Snowflake temporaryThreadId;
    Snowflake temporaryThreadAccount;
};
} // namespace UI
} // namespace Acheron
