#include "ChannelTreeView.hpp"
#include "ChannelFilterProxyModel.hpp"
#include "ChannelTreeModel.hpp"

#include <QContextMenuEvent>
#include <QMenu>

namespace Acheron {
namespace UI {

ChannelTreeView::ChannelTreeView(QWidget *parent)
    : QTreeView(parent)
{
}

void ChannelTreeView::performDefaultExpansion()
{
    std::function<void(const QModelIndex &)> walk = [this, &walk](const QModelIndex &parent) {
        int rows = model()->rowCount(parent);
        for (int i = 0; i < rows; ++i) {
            QModelIndex idx = model()->index(i, 0, parent);
            auto nodeType = static_cast<ChannelNode::Type>(idx.data(ChannelTreeModel::TypeRole).toInt());
            if (nodeType == ChannelNode::Type::Category || nodeType == ChannelNode::Type::Account)
                expand(idx);
            if (model()->hasChildren(idx))
                walk(idx);
        }
    };
    walk({});
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
    } else if (model()->hasChildren(proxyIndex)) {
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

    if (isVoiceChannel) {
        QAction *joinAction = menu.addAction(tr("Join Voice Channel"));
        connect(joinAction, &QAction::triggered, this, [this, proxyIndex]() {
            emit joinVoiceChannelRequested(proxyIndex);
        });

        Core::Snowflake acctId = findAccountIdForIndex(sourceIndex);
        bool inVoice = isAccountInVoice(acctId);

        QAction *disconnectAction = menu.addAction(tr("Disconnect from Voice"));
        disconnectAction->setEnabled(inVoice);
        connect(disconnectAction, &QAction::triggered, this, [this, proxyIndex]() {
            emit disconnectVoiceRequested(proxyIndex);
        });

        menu.addSeparator();
    }

    QAction *markReadAction = menu.addAction(tr("Mark As Read"));
    markReadAction->setEnabled(isUnread);

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
