#include "ChannelTreeView.hpp"
#include "ChannelFilterProxyModel.hpp"
#include "ChannelTreeModel.hpp"

#include <QContextMenuEvent>
#include <QMenu>

namespace Acheron {
namespace UI {

static bool isAlwaysExpanded(ChannelNode::Type type)
{
    return type == ChannelNode::Type::Forum;
}

ChannelTreeView::ChannelTreeView(QWidget *parent)
    : QTreeView(parent)
{
    connect(this, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
        auto nodeType = static_cast<ChannelNode::Type>(index.data(ChannelTreeModel::TypeRole).toInt());
        if (isAlwaysExpanded(nodeType))
            expand(index);
    });
}

void ChannelTreeView::setModel(QAbstractItemModel *m)
{
    QTreeView::setModel(m);
    if (m)
        connect(m, &QAbstractItemModel::rowsInserted, this, &ChannelTreeView::onRowsInserted);
}

void ChannelTreeView::onRowsInserted(const QModelIndex &parent, int first, int last)
{
    if (parent.isValid()) {
        auto parentType = static_cast<ChannelNode::Type>(parent.data(ChannelTreeModel::TypeRole).toInt());
        if ((parentType == ChannelNode::Type::VoiceChannel ||
             parentType == ChannelNode::Type::Channel ||
             isAlwaysExpanded(parentType)) &&
            !isExpanded(parent))
            expand(parent);
    }

    for (int row = first; row <= last; row++) {
        QModelIndex index = model()->index(row, 0, parent);
        auto nodeType = static_cast<ChannelNode::Type>(index.data(ChannelTreeModel::TypeRole).toInt());
        if (isAlwaysExpanded(nodeType) && !isExpanded(index))
            expand(index);
    }
}

void ChannelTreeView::performDefaultExpansion()
{
    std::function<void(const QModelIndex &)> walk = [this, &walk](const QModelIndex &parent) {
        int rows = model()->rowCount(parent);
        for (int i = 0; i < rows; ++i) {
            QModelIndex idx = model()->index(i, 0, parent);
            auto nodeType = static_cast<ChannelNode::Type>(idx.data(ChannelTreeModel::TypeRole).toInt());
            if (nodeType == ChannelNode::Type::Category || nodeType == ChannelNode::Type::Account ||
                nodeType == ChannelNode::Type::VoiceChannel || isAlwaysExpanded(nodeType))
                expand(idx);
            else if (nodeType == ChannelNode::Type::Channel && model()->hasChildren(idx))
                expand(idx);
            if (model()->hasChildren(idx))
                walk(idx);
        }
    };
    walk(rootIndex());
}

void ChannelTreeView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->accept();
        return;
    }

    bool handledCategory = handleMouseEventForExpansion(event);

    if (!handledCategory)
        QTreeView::mousePressEvent(event);
}

void ChannelTreeView::mouseDoubleClickEvent(QMouseEvent *event)
{
    handleMouseEventForExpansion(event);
    event->accept(); // shut up
}

bool ChannelTreeView::handleMouseEventForExpansion(QMouseEvent *event)
{
    QModelIndex proxyIndex = indexAt(event->pos());
    if (!proxyIndex.isValid())
        return false;

    auto *proxy = qobject_cast<ChannelFilterProxyModel *>(model());
    if (!proxy)
        return false;

    QModelIndex sourceIndex = proxy->mapToSource(proxyIndex);
    auto *sourceModel = qobject_cast<ChannelTreeModel *>(proxy->sourceModel());
    if (!sourceModel)
        return false;

    auto nodeType = static_cast<ChannelNode::Type>(sourceIndex.data(ChannelTreeModel::TypeRole).toInt());
    if (nodeType == ChannelNode::Type::Category) {
        sourceModel->toggleCollapsed(sourceIndex);
        proxy->invalidateFilter();
        return true;
    } else if (nodeType != ChannelNode::Type::Channel && !isAlwaysExpanded(nodeType) && model()->hasChildren(proxyIndex)) {
        setExpanded(proxyIndex, !isExpanded(proxyIndex));
        return true;
    }
    return false;
}

void ChannelTreeView::contextMenuEvent(QContextMenuEvent *event)
{
    QModelIndex proxyIndex = indexAt(event->pos());
    if (!proxyIndex.isValid())
        return;

    auto *proxy = qobject_cast<ChannelFilterProxyModel *>(model());
    if (!proxy)
        return;

    QModelIndex sourceIndex = proxy->mapToSource(proxyIndex);
    auto nodeType = static_cast<ChannelNode::Type>(sourceIndex.data(ChannelTreeModel::TypeRole).toInt());

    if (nodeType == ChannelNode::Type::Root || nodeType == ChannelNode::Type::Account ||
        nodeType == ChannelNode::Type::DMHeader)
        return;

    bool isUnread = sourceIndex.data(ChannelTreeModel::IsUnreadRole).toBool();
    int mentionCount = sourceIndex.data(ChannelTreeModel::MentionCountRole).toInt();
    bool isChannel = (nodeType == ChannelNode::Type::Channel ||
                      nodeType == ChannelNode::Type::DMChannel);
    bool isVoiceChannel = (nodeType == ChannelNode::Type::VoiceChannel);

    QMenu menu(this);

    if (isChannel) {
        QAction *openTabAction = menu.addAction(tr("Open in New Tab"));
        connect(openTabAction, &QAction::triggered, this, [this, proxyIndex]() {
            emit openInNewTabRequested(proxyIndex);
        });
        menu.addSeparator();
    }

    auto parentType = static_cast<ChannelNode::Type>(sourceIndex.parent().data(ChannelTreeModel::TypeRole).toInt());
    if (nodeType == ChannelNode::Type::Thread && parentType == ChannelNode::Type::Channel) {
        bool joined = sourceIndex.data(ChannelTreeModel::ThreadJoinedRole).toBool();
        QAction *action = menu.addAction(joined ? tr("Leave Thread") : tr("Join Thread"));
        connect(action, &QAction::triggered, this, [this, proxyIndex, joined]() {
            if (joined)
                emit leaveThreadRequested(proxyIndex);
            else
                emit joinThreadRequested(proxyIndex);
        });
        menu.addSeparator();
    }

    bool isDMChannel = (nodeType == ChannelNode::Type::DMChannel);

    if (isVoiceChannel || isDMChannel) {
        QString joinText = isDMChannel ? tr("Join Call") : tr("Join Voice Channel");
        QAction *joinAction = menu.addAction(joinText);
#ifdef ACHERON_NO_VOICE
        joinAction->setEnabled(false);
#else
        connect(joinAction, &QAction::triggered, this, [this, proxyIndex]() {
            emit joinVoiceChannelRequested(proxyIndex);
        });
#endif

        Core::Snowflake acctId = findAccountIdForIndex(sourceIndex);
        bool inVoice = isAccountInVoice(acctId);

        QAction *disconnectAction = menu.addAction(tr("Disconnect from Voice"));
#ifdef ACHERON_NO_VOICE
        disconnectAction->setEnabled(false);
#else
        disconnectAction->setEnabled(inVoice);
        connect(disconnectAction, &QAction::triggered, this, [this, proxyIndex]() {
            emit disconnectVoiceRequested(proxyIndex);
        });
#endif

        menu.addSeparator();
    }

    if (nodeType == ChannelNode::Type::Server) {
        Core::Snowflake ownerId(sourceIndex.data(ChannelTreeModel::OwnerIdRole).toULongLong());
        Core::Snowflake accountId = findAccountIdForIndex(sourceIndex);

        menu.addSeparator();
        QAction *leaveAction = menu.addAction(tr("Leave"));
        leaveAction->setEnabled(ownerId != accountId);
        connect(leaveAction, &QAction::triggered, this, [this, sourceIndex]() {
            Core::Snowflake accountId = findAccountIdForIndex(sourceIndex);
            Core::Snowflake guildId(sourceIndex.data(ChannelTreeModel::IdRole).toULongLong());
            emit leaveGuildRequested(accountId, guildId);
        });
    }

    QAction *markReadAction = menu.addAction(tr("Mark As Read"));
    markReadAction->setEnabled(isUnread || mentionCount > 0);

    connect(markReadAction, &QAction::triggered, this, [this, proxyIndex]() {
        emit markAsReadRequested(proxyIndex);
    });

    menu.exec(event->globalPos());
}

void ChannelTreeView::setAccountVoiceChannel(Core::Snowflake accountId, Core::Snowflake channelId)
{
    if (channelId.isValid())
        accountVoiceChannels[accountId] = channelId;
    else
        accountVoiceChannels.remove(accountId);
}

bool ChannelTreeView::isAccountInVoice(Core::Snowflake accountId) const
{
    return accountVoiceChannels.contains(accountId);
}

Core::Snowflake ChannelTreeView::findAccountIdForIndex(const QModelIndex &sourceIndex) const
{
    QModelIndex current = sourceIndex;
    while (current.isValid()) {
        auto type = static_cast<ChannelNode::Type>(current.data(ChannelTreeModel::TypeRole).toInt());
        if (type == ChannelNode::Type::Account)
            return Core::Snowflake(current.data(ChannelTreeModel::IdRole).toULongLong());
        current = current.parent();
    }
    return Core::Snowflake::Invalid;
}

} // namespace UI
} // namespace Acheron
