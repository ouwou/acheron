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

                QModelIndex idx = index(i, 0);
                emit dataChanged(idx, idx, { Qt::DisplayRole, AccountObjectRole });
                break;
            }
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
        QString text = acc.displayName;

        if (!acc.username.isEmpty() && acc.username != "unknown")
            text += " (" + acc.username + ")";

        if (acc.state == ConnectionState::Connected)
            text += " [Connected]";
        else if (acc.state == ConnectionState::Connecting)
            text += " [Connecting...]";
        return text;
    }
    case Qt::DecorationRole:
        return acc.avatar.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation);

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