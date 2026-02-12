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

    QMenu menu(this);
    QAction *markReadAction = menu.addAction(tr("Mark As Read"));
    markReadAction->setEnabled(isUnread);

    connect(markReadAction, &QAction::triggered, this, [this, proxyIndex]() {
        emit markAsReadRequested(proxyIndex);
    });

    menu.exec(event->globalPos());
}

} // namespace UI
} // namespace Acheron
