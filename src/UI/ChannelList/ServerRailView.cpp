#include "ServerRailView.hpp"

#include "ServerRailModel.hpp"

#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>

namespace Acheron {
namespace UI {

ServerRailView::ServerRailView(QWidget *parent)
    : QListView(parent)
{
    setSelectionMode(QAbstractItemView::NoSelection);
    setFocusPolicy(Qt::NoFocus);
    setUniformItemSizes(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
}

void ServerRailView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return QListView::mousePressEvent(event);

    QModelIndex idx = indexAt(event->pos());
    if (!idx.isValid())
        return QListView::mousePressEvent(event);

    auto kind = static_cast<ServerRailModel::Kind>(idx.data(ServerRailModel::KindRole).toInt());
    Core::Snowflake accountId(idx.data(ServerRailModel::AccountIdRole).toULongLong());
    Core::Snowflake id(idx.data(ServerRailModel::IdRole).toULongLong());

    switch (kind) {
    case ServerRailModel::Kind::AccountHome:
        emit accountHomeClicked(accountId);
        break;
    case ServerRailModel::Kind::Server:
        emit guildClicked(accountId, id);
        break;
    case ServerRailModel::Kind::Folder:
        emit folderToggleClicked(accountId, id);
        break;
    }

    event->accept();
}

void ServerRailView::contextMenuEvent(QContextMenuEvent *event)
{
    QModelIndex idx = indexAt(event->pos());
    if (!idx.isValid())
        return;

    auto kind = static_cast<ServerRailModel::Kind>(idx.data(ServerRailModel::KindRole).toInt());
    if (kind != ServerRailModel::Kind::Folder && kind != ServerRailModel::Kind::Server)
        return;

    Core::Snowflake accountId(idx.data(ServerRailModel::AccountIdRole).toULongLong());
    Core::Snowflake id(idx.data(ServerRailModel::IdRole).toULongLong());
    const bool isFolder = kind == ServerRailModel::Kind::Folder;
    const bool isUnread = idx.data(ServerRailModel::IsUnreadRole).toBool();
    const int mentions = idx.data(ServerRailModel::MentionCountRole).toInt();

    QMenu menu(this);
    QAction *markRead = menu.addAction(tr("Mark As Read"));
    markRead->setEnabled(isUnread || mentions > 0);
    connect(markRead, &QAction::triggered, this, [this, accountId, id, isFolder]() {
        emit markAsReadRequested(accountId, id, isFolder);
    });

    if (!isFolder) {
        Core::Snowflake ownerId(idx.data(ServerRailModel::OwnerIdRole).toULongLong());

        menu.addSeparator();
        QAction *leaveAction = menu.addAction(tr("Leave"));
        leaveAction->setEnabled(ownerId != accountId);
        connect(leaveAction, &QAction::triggered, this, [this, accountId, id]() {
            emit leaveGuildRequested(accountId, id);
        });
    }

    menu.exec(event->globalPos());
}

void ServerRailView::mouseMoveEvent(QMouseEvent *event)
{
    viewport()->setCursor(indexAt(event->pos()).isValid() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QListView::mouseMoveEvent(event);
}

} // namespace UI
} // namespace Acheron
