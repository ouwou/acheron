#include "AccountsModel.hpp"

#include "Storage/AccountRepository.hpp"

#include "Core/Session.hpp"

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
    AccountRepository repo;
    repo.saveAccount(account);

    beginInsertRows(QModelIndex(), accounts.size(), accounts.size());
    accounts.append(account);
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

} // namespace UI
} // namespace Acheron