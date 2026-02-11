#include "AccountsModel.hpp"

#include "Storage/AccountRepository.hpp"
#include "Core/TokenStore.hpp"

#include "Core/Session.hpp"
#include <QMimeData>

namespace Acheron {
namespace UI {

using namespace Acheron::Core;
using namespace Acheron::Storage;

AccountsModel::AccountsModel(Core::Session *session, QObject *parent)
    : QAbstractListModel(parent), session(session)
{

    AccountRepository repo;
    accounts = repo.getAllAccounts();

    connect(session, &Session::connectionStateChanged, this,
            [this](Snowflake id, ConnectionState newState) {
                for (int i = 0; i < accounts.size(); ++i) {
                    if (accounts[i].id == id) {
                        accounts[i].state = newState;

                        QModelIndex idx = index(i, 0);
                        emit dataChanged(idx, idx, { Qt::DisplayRole, ConnectionStateRole });
                        break;
                    }
                }
            });

    connect(session, &Session::accountDetailsUpdated, this, [this](const AccountInfo &info) {
        for (int i = 0; i < accounts.size(); ++i) {
            if (accounts[i].id == info.id) {
                accounts[i].username = info.username;
                accounts[i].displayName = info.displayName;
                accounts[i].avatar = info.avatar;

                QModelIndex idx = index(i, 0);
                emit dataChanged(idx, idx, { Qt::DisplayRole, AccountObjectRole });
                break;
            }
        }
    });

    connect(session->getImageManager(), &Core::ImageManager::imageFetched, this,
            [this](const QUrl &url, const QSize &size, const QPixmap &pixmap) {
                auto values = pendingRequests.values(url);
                for (const auto &index : values) {
                    if (index.isValid())
                        emit dataChanged(index, index, { Qt::DecorationRole });
                }
            });
}

int AccountsModel::rowCount(const QModelIndex &parent) const
{
    return accounts.size();
}

QVariant AccountsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= accounts.size())
        return {};

    const auto &acc = accounts[index.row()];

    switch (role) {
    case Qt::DisplayRole: {
        QString text = acc.getEntryString();

        if (acc.state == ConnectionState::Connected)
            text += " [Connected]";
        else if (acc.state == ConnectionState::Connecting)
            text += " [Connecting...]";
        return text;
    }
    case Qt::DecorationRole: {
        const QSize desiredSize(32, 32);
        QUrl TEMPORARY = QUrl(QString("https://cdn.discordapp.com/avatars/%1/%2.png?size=%3")
                                      .arg(quint64(acc.id))
                                      .arg(acc.avatar)
                                      .arg(desiredSize.width()));

        QPixmap pixmap = session->getImageManager()->get(TEMPORARY, desiredSize);

        if (!session->getImageManager()->isCached(TEMPORARY, desiredSize)) {
            bool alreadyWaiting = false;
            auto it = pendingRequests.constFind(TEMPORARY);
            while (it != pendingRequests.cend() && it.key() == TEMPORARY) {
                if (it.value() == index) {
                    alreadyWaiting = true;
                    break;
                }
                it++;
            }

            if (!alreadyWaiting)
                pendingRequests.insert(TEMPORARY, QPersistentModelIndex(index));
        }

        return pixmap;
    }
    case AccountObjectRole:
        return QVariant::fromValue((void *)&acc);

    case ConnectionStateRole:
        return QVariant::fromValue(acc.state);

    default:
        return {};
    }
}

void AccountsModel::addAccount(const AccountInfo &account)
{
    AccountInfo newAccount = account;

    int maxOrder = -1;
    for (const auto &acc : accounts) {
        if (acc.displayOrder > maxOrder)
            maxOrder = acc.displayOrder;
    }
    newAccount.displayOrder = maxOrder + 1;

    Core::TokenStore::saveToken(newAccount.id, newAccount.token);

    AccountRepository repo;
    repo.saveAccount(newAccount);

    // Don't keep the token in the in-memory model
    newAccount.token.clear();

    beginInsertRows(QModelIndex(), accounts.size(), accounts.size());
    accounts.append(newAccount);
    endInsertRows();
}

void AccountsModel::removeAccount(int row)
{
    if (row < 0 || row >= accounts.size())
        return;

    quint64 idToRemove = accounts[row].id;
    AccountRepository repo;
    repo.removeAccount(idToRemove);

    beginRemoveRows(QModelIndex(), row, row);
    accounts.removeAt(row);
    endRemoveRows();
}

void AccountsModel::setConnectionState(int row, ConnectionState state)
{
    if (row < 0 || row >= accounts.size())
        return;

    accounts[row].state = state;

    QModelIndex idx = index(row, 0);

    emit dataChanged(idx, idx, { Qt::DisplayRole, ConnectionStateRole, AccountObjectRole });
}

Qt::ItemFlags AccountsModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags defaultFlags = QAbstractListModel::flags(index);
    if (index.isValid())
        return Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | defaultFlags;
    return Qt::ItemIsDropEnabled | defaultFlags;
}

Qt::DropActions AccountsModel::supportedDropActions() const
{
    return Qt::MoveAction;
}

QStringList AccountsModel::mimeTypes() const
{
    return { "application/x-acheron-account-index" };
}

QMimeData *AccountsModel::mimeData(const QModelIndexList &indexes) const
{
    if (indexes.isEmpty())
        return nullptr;

    QMimeData *mimeData = new QMimeData();
    QByteArray encodedData;
    QDataStream stream(&encodedData, QIODevice::WriteOnly);

    for (const QModelIndex &index : indexes) {
        if (index.isValid())
            stream << index.row();
    }

    mimeData->setData("application/x-acheron-account-index", encodedData);
    return mimeData;
}

bool AccountsModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                 const QModelIndex &parent)
{
    if (!data->hasFormat("application/x-acheron-account-index"))
        return false;

    if (action == Qt::IgnoreAction)
        return true;

    int targetRow = row;
    if (targetRow == -1) {
        if (parent.isValid())
            targetRow = parent.row();
        else
            targetRow = rowCount();
    }

    QByteArray encodedData = data->data("application/x-acheron-account-index");
    QDataStream stream(&encodedData, QIODevice::ReadOnly);

    int sourceRow;
    stream >> sourceRow;

    if (sourceRow == targetRow || sourceRow + 1 == targetRow)
        return false;

    int actualTargetRow = targetRow;
    if (sourceRow < targetRow)
        actualTargetRow--;

    if (actualTargetRow < 0 || actualTargetRow >= accounts.size() || sourceRow < 0 ||
        sourceRow >= accounts.size())
        return false;

    beginMoveRows(QModelIndex(), sourceRow, sourceRow, QModelIndex(),
                  sourceRow < actualTargetRow ? actualTargetRow + 1 : actualTargetRow);
    accounts.move(sourceRow, actualTargetRow);
    endMoveRows();

    AccountRepository repo;
    for (int i = 0; i < accounts.size(); ++i) {
        accounts[i].displayOrder = i;
        repo.updateDisplayOrder(accounts[i].id, i);
    }

    return true;
}

} // namespace UI
} // namespace Acheron