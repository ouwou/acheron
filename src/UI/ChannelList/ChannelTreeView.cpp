#include "ChannelTreeView.hpp"

namespace Acheron {
namespace UI {

ChannelTreeView::ChannelTreeView(QWidget *parent) : QTreeView(parent) { }

void ChannelTreeView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        handleMouseEventForExpansion(event);
    QTreeView::mousePressEvent(event);
}

void ChannelTreeView::mouseDoubleClickEvent(QMouseEvent *event)
{
    handleMouseEventForExpansion(event);
    event->accept(); // shut up
}

void ChannelTreeView::handleMouseEventForExpansion(QMouseEvent *event)
{
    QModelIndex index = indexAt(event->pos());

    if (index.isValid() && model()->hasChildren(index))
        setExpanded(index, !isExpanded(index));
}

} // namespace UI
} // namespace Acheron
