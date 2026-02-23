#include "AccountsWindow.hpp"

#include <QRandomGenerator>

#include "Core/Session.hpp"
#include "Core/TokenUtils.hpp"

namespace Acheron {
namespace UI {

using namespace Acheron::Core;

AccountsWindow::AccountsWindow(Session *session, AccountsModel *sharedModel, QWidget *parent)
    : QWidget(parent), session(session)
{
    setWindowTitle(tr("Manage Accounts"));
    resize(700, 450);

    this->model = sharedModel;

    setupUi();
}

void AccountsWindow::setupUi()
{
    QHBoxLayout *mainLayout = new QHBoxLayout(this);

    QWidget *leftPane = new QWidget(this);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPane);

    listView = new QListView(this);
    listView->setModel(model);
    listView->setIconSize(QSize(32, 32));
    listView->setDragEnabled(true);
    listView->setAcceptDrops(true);
    listView->setDropIndicatorShown(true);
    listView->setDragDropMode(QAbstractItemView::InternalMove);

    listView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(listView, &QListView::customContextMenuRequested, this,
            &AccountsWindow::onContextMenuRequested);

    leftLayout->addWidget(listView);

    QHBoxLayout *btnLayout = new QHBoxLayout();
    QPushButton *addBtn = new QPushButton(tr("Add"), this);
    removeButton = new QPushButton(tr("Remove"), this);
    removeButton->setEnabled(false);
    btnLayout->addWidget(addBtn);
    btnLayout->addWidget(removeButton);
    leftLayout->addLayout(btnLayout);

    QWidget *rightPane = new QWidget(this);
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPane);

    emptyStateLabel = new QLabel(tr("Select an account"), rightPane);
    emptyStateLabel->setAlignment(Qt::AlignCenter);
    rightLayout->addWidget(emptyStateLabel);

    detailsContainer = new QWidget(rightPane);

    QVBoxLayout *containerLayout = new QVBoxLayout(detailsContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);

    detailAvatar = new QLabel(detailsContainer);
    detailAvatar->setFixedSize(64, 64);
    detailAvatar->setScaledContents(true);

    QHBoxLayout *avLayout = new QHBoxLayout();
    avLayout->addStretch();
    avLayout->addWidget(detailAvatar);
    avLayout->addStretch();

    containerLayout->addLayout(avLayout);

    QFormLayout *form = new QFormLayout();

    detailDisplayName = new QLabel(detailsContainer);
    detailUsername = new QLabel(detailsContainer);
    detailId = new QLabel(detailsContainer);
    detailStatus = new QLabel(detailsContainer);
    detailStatus->setStyleSheet("font-weight: bold;");

    QHBoxLayout *actionBtnLayout = new QHBoxLayout();
    connectButton = new QPushButton(tr("Connect"), detailsContainer);
    disconnectButton = new QPushButton(tr("Disconnect"), detailsContainer);
    actionBtnLayout->addWidget(connectButton);
    actionBtnLayout->addWidget(disconnectButton);

    form->addRow(tr("Display Name:"), detailDisplayName);
    form->addRow(tr("Status:"), detailStatus);
    form->addRow(tr("Username:"), detailUsername);
    form->addRow(tr("User ID:"), detailId);
    form->addRow(tr("Actions:"), actionBtnLayout);

    containerLayout->addLayout(form);
    containerLayout->addStretch();
    rightLayout->addWidget(detailsContainer);

    detailsContainer->setVisible(false);

    QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(leftPane);
    splitter->addWidget(rightPane);
    mainLayout->addWidget(splitter);

    connect(addBtn, &QPushButton::clicked, this, &AccountsWindow::onAddClicked);
    connect(removeButton, &QPushButton::clicked, this, &AccountsWindow::onRemoveClicked);

    connect(listView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &AccountsWindow::onSelectionChanged);

    connect(model, &QAbstractListModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight) {
                QModelIndex current = listView->selectionModel()->currentIndex();
                if (current.isValid() && current.row() >= topLeft.row() &&
                    current.row() <= bottomRight.row()) {
                    void *ptr = current.data(AccountsModel::AccountObjectRole).value<void *>();
                    updateDetails(static_cast<AccountInfo *>(ptr));
                }
            });

    connect(connectButton, &QPushButton::clicked, this, &AccountsWindow::onConnectClicked);
    connect(disconnectButton, &QPushButton::clicked, this, &AccountsWindow::onDisconnectClicked);
}

void AccountsWindow::performConnect(int row)
{
    QModelIndex idx = model->index(row, 0);
    if (!idx.isValid())
        return;

    auto *info =
            static_cast<AccountInfo *>(idx.data(AccountsModel::AccountObjectRole).value<void *>());
    if (!info)
        return;

    session->connectAccount(info->id);
}

void AccountsWindow::performDisconnect(int row)
{
    QModelIndex idx = model->index(row, 0);
    if (!idx.isValid())
        return;

    auto *info =
            static_cast<AccountInfo *>(idx.data(AccountsModel::AccountObjectRole).value<void *>());
    if (!info)
        return;

    session->disconnectAccount(info->id);
}

void AccountsWindow::onAddClicked()
{
    AddAccountDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    QString token = dlg.getToken();
    if (token.isEmpty())
        return;

    Snowflake userId = TokenUtils::getIdAndCheckToken(token);
    if (!userId.isValid()) {
        // todo: complain
        return;
    }

    AccountInfo acc;
    acc.id = userId;

    acc.displayName = "unknown";
    acc.username = "unknown";
    acc.token = token;

    // acc.avatar =

    model->addAccount(acc);
}

void AccountsWindow::onContextMenuRequested(const QPoint &pos)
{
    QModelIndex index = listView->indexAt(pos);
    if (!index.isValid())
        return;

    ConnectionState state = index.data(AccountsModel::ConnectionStateRole).value<ConnectionState>();

    QMenu menu(this);
    QAction *actConnect = menu.addAction("Connect");
    QAction *actDisconnect = menu.addAction("Disconnect");
    menu.addSeparator();
    QAction *actRemove = menu.addAction("Remove Account");

    if (state == ConnectionState::Connected || state == ConnectionState::Connecting)
        actConnect->setEnabled(false);
#ifndef ACHERON_MULTI_ACCOUNT
    else if (session->hasActiveConnection())
        actConnect->setEnabled(false);
#endif
    if (state == ConnectionState::Disconnected)
        actDisconnect->setEnabled(false);

    QAction *selected = menu.exec(listView->viewport()->mapToGlobal(pos));

    if (selected == actConnect)
        performConnect(index.row());
    else if (selected == actDisconnect)
        performDisconnect(index.row());
    else if (selected == actRemove)
        model->removeAccount(index.row());
}

void AccountsWindow::onSelectionChanged(const QModelIndex &current, const QModelIndex &prev)
{
    if (!current.isValid()) {
        detailsContainer->setVisible(false);
        emptyStateLabel->setVisible(true);
        removeButton->setEnabled(false);
        return;
    }
    void *ptr = current.data(AccountsModel::AccountObjectRole).value<void *>();
    updateDetails(static_cast<AccountInfo *>(ptr));
    detailsContainer->setVisible(true);
    emptyStateLabel->setVisible(false);
    removeButton->setEnabled(true);
}

void AccountsWindow::updateDetails(const AccountInfo *info)
{
    if (!info)
        return;

    const QSize desiredSize(64, 64);
    QUrl TEMPORARY = QUrl(QString("https://cdn.discordapp.com/avatars/%1/%2.png?size=%3")
                                  .arg(quint64(info->id))
                                  .arg(info->avatar)
                                  .arg(desiredSize.width()));

    session->getImageManager()->assign(detailAvatar, TEMPORARY, desiredSize);
    detailDisplayName->setText(info->displayName);
    detailUsername->setText(info->username);
    detailId->setText(QString::number(info->id));

    switch (info->state) {
    case ConnectionState::Disconnected:
        detailStatus->setText(tr("Disconnected"));
        detailStatus->setStyleSheet("color: red; font-weight: bold;");
#ifdef ACHERON_MULTI_ACCOUNT
        connectButton->setEnabled(true);
#else
        connectButton->setEnabled(!session->hasActiveConnection());
#endif
        disconnectButton->setEnabled(false);
        break;
    case ConnectionState::Connecting:
        detailStatus->setText(tr("Connecting..."));
        detailStatus->setStyleSheet("color: orange; font-weight: bold;");
        connectButton->setEnabled(false);
        disconnectButton->setEnabled(false);
        break;
    case ConnectionState::Connected:
        detailStatus->setText(tr("Connected"));
        detailStatus->setStyleSheet("color: green; font-weight: bold;");
        connectButton->setEnabled(false);
        disconnectButton->setEnabled(true);
        break;
    case ConnectionState::Disconnecting:
        detailStatus->setText(tr("Disconnecting..."));
        detailStatus->setStyleSheet("color: orange; font-weight: bold;");
        connectButton->setEnabled(false);
        disconnectButton->setEnabled(false);
        break;
    }
}

void AccountsWindow::onConnectClicked()
{
    QModelIndex idx = listView->selectionModel()->currentIndex();
    if (idx.isValid())
        performConnect(idx.row());
}

void AccountsWindow::onDisconnectClicked()
{
    QModelIndex idx = listView->selectionModel()->currentIndex();
    if (idx.isValid())
        performDisconnect(idx.row());
}

void AccountsWindow::onRemoveClicked()
{
    QModelIndex idx = listView->selectionModel()->currentIndex();
    if (idx.isValid())
        model->removeAccount(idx.row());
}

} // namespace UI
} // namespace Acheron
