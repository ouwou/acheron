#pragma once

#include <QtWidgets>

#include "ChannelNode.hpp"
#include "Core/AccountInfo.hpp"

#include "Core/Snowflake.hpp"
#include "Core/Session.hpp"
#include "Discord/Events.hpp"

using Acheron::Core::Session;
using Acheron::Core::Snowflake;

namespace Acheron {
namespace UI {
class ChannelTreeModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    ChannelTreeModel(Session *session, QObject *parent = nullptr);

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

private:
    ChannelNode *nodeFromIndex(const QModelIndex &index) const;
    QModelIndex indexForNode(ChannelNode *node) const;
    std::unique_ptr<ChannelNode> createGuildNode(const Discord::GatewayGuild &guild);

private:
    Session *session;

    std::unique_ptr<ChannelNode> root;
    QHash<Snowflake, ChannelNode *> accountNodes;
    mutable QMultiMap<QUrl, QPersistentModelIndex> pendingRequests;
};
} // namespace UI
} // namespace Acheron
