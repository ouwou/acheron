#pragma once

#include <QAbstractListModel>
#include <QVector>
#include "Core/AccountInfo.hpp"

namespace Acheron {

namespace Core {
class Session;
}

namespace UI {

class AccountsModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit AccountsModel(Core::Session *session, QObject *parent = nullptr);

    enum Roles { AccountObjectRole = Qt::UserRole + 1, ConnectionStateRole };

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    void addAccount(const Core::AccountInfo &account);
    void removeAccount(int row);

    void setConnectionState(int row, Core::ConnectionState state);

private:
    Core::Session *session;

    QVector<Core::AccountInfo> accounts;
};

} // namespace UI
} // namespace Acheron