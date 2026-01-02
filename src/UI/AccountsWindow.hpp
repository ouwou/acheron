#pragma once

#include <QtWidgets>

#include "AccountsModel.hpp"
#include "Dialogs/AddAccountDialog.hpp"

namespace Acheron {

namespace Core {
class Session;
}

namespace UI {

class AccountsWindow : public QWidget
{
    Q_OBJECT
public:
    explicit AccountsWindow(Core::Session *session, AccountsModel *sharedModel,
                            QWidget *parent = nullptr);

private slots:
    void onSelectionChanged(const QModelIndex &current, const QModelIndex &previous);
    void onAddClicked();
    void onRemoveClicked();
    void onContextMenuRequested(const QPoint &pos);
    void onConnectClicked();
    void onDisconnectClicked();

private:
    Core::Session *session;

    AccountsModel *model;
    QListView *listView;

    // Details Panel
    QLabel *detailAvatar;
    QLabel *detailDisplayName;
    QLabel *detailUsername;
    QLabel *detailId;
    QLabel *detailStatus;
    QWidget *detailsContainer;
    QLabel *emptyStateLabel;

    QPushButton *removeButton;
    QPushButton *connectButton;
    QPushButton *disconnectButton;

    void setupUi();
    void updateDetails(const Acheron::Core::AccountInfo *info);
    void performConnect(int row);
    void performDisconnect(int row);

protected:
};

} // namespace UI
} // namespace Acheron