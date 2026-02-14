#include "MemberListView.hpp"

namespace Acheron {
namespace UI {

MemberListView::MemberListView(QWidget *parent)
    : QListView(parent)
{
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setSelectionMode(QAbstractItemView::NoSelection);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setUniformItemSizes(false);

    setStyleSheet(
            "QListView {"
            "  background-color: palette(window);"
            "  border: none;"
            "}"
            "QScrollBar::handle:vertical {"
            "  min-height: 40px;"
            "}");
}

void MemberListView::scrollContentsBy(int dx, int dy)
{
    QListView::scrollContentsBy(dx, dy);
    emitVisibleRange();
}

void MemberListView::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);
    emitVisibleRange();
}

void MemberListView::emitVisibleRange()
{
    if (!model() || model()->rowCount() == 0)
        return;

    QModelIndex topIndex = indexAt(viewport()->rect().topLeft());
    QModelIndex bottomIndex = indexAt(viewport()->rect().bottomLeft());

    int first = topIndex.isValid() ? topIndex.row() : 0;
    int last = bottomIndex.isValid() ? bottomIndex.row() : model()->rowCount() - 1;

    if (first != lastFirstVisible || last != lastLastVisible) {
        lastFirstVisible = first;
        lastLastVisible = last;
        emit visibleRangeChanged(first, last);
    }
}

} // namespace UI
} // namespace Acheron
