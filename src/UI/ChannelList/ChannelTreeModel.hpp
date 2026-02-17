#pragma once

#include <QtWidgets>

#include "ChannelNode.hpp"
#include "Core/AccountInfo.hpp"

#include "Core/Snowflake.hpp"
#include "Core/Session.hpp"
#include "Discord/Events.hpp"

using Acheron::Core::Session;
using Acheron::Core::Snowflake;

namespace Acheron::Core {
struct ChannelReadState;
class ClientInstance;
} // namespace Acheron::Core

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

    ChannelNode *getAccountNodeFor(ChannelNode *node);

    ChannelNode *nodeFromIndex(const QModelIndex &index) const;
    void addChannel(const Discord::ChannelCreate &event, Snowflake accountId);
    void updateChannel(const Discord::ChannelUpdate &update, Snowflake accountId);
    void deleteChannel(const Discord::ChannelDelete &event, Snowflake accountId);
    void invalidateGuildData(Snowflake guildId);
    void updateReadState(Snowflake channelId, Snowflake accountId);
    void updateGuildSettings(Snowflake guildId, Snowflake accountId);
    void updateChannelLastMessageId(Snowflake channelId, Snowflake messageId, Snowflake accountId);
    void toggleCollapsed(const QModelIndex &index);

    QList<QPair<Snowflake, Snowflake>> getMarkableChannels(const QModelIndex &index);
    ChannelNode *findChannelTreeNode(Snowflake channelId);

private:
    void initChannelReadStates(ChannelNode *node, Core::ClientInstance *instance);
    void updateChildrenReadState(ChannelNode *node, Snowflake guildId,
                                 Core::ClientInstance *instance);
    static void collectMarkableChannels(ChannelNode *node,
                                        QList<QPair<Snowflake, Snowflake>> &out);
    void applyChannelReadState(ChannelNode *node, const Core::ChannelReadState &state);
    static void aggregateChildren(ChannelNode *node);
    void recomputeSubtreeAggregates(ChannelNode *root);
    void updateNodeAggregates(ChannelNode *node);
    QModelIndex indexForNode(ChannelNode *node) const;
    std::unique_ptr<ChannelNode> createGuildNode(const Discord::GatewayGuild &guild);
    ChannelNode *findChannelTreeNode(Snowflake channelId, ChannelNode *root);
    ChannelNode *findGuildNode(ChannelNode *node);
    ChannelNode *findGuildNodeById(Snowflake guildId, ChannelNode *accountNode);
    ChannelNode *findCategoryNode(Snowflake categoryId, ChannelNode *guildNode);
    void emitDataChangedRecursive(const QModelIndex &index);

private:
    Session *session;

    std::unique_ptr<ChannelNode> root;
    QHash<Snowflake, ChannelNode *> accountNodes;
    mutable QMultiMap<QUrl, QPersistentModelIndex> pendingRequests;
};
} // namespace UI
} // namespace Acheron
