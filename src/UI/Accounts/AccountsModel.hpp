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

    enum Roles {
        AccountObjectRole = Qt::UserRole + 1,
        ConnectionStateRole,
        AutoConnectRole,
    };

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;

    void addAccount(const Core::AccountInfo &account);
    void removeAccount(int row);

    void setConnectionState(int row, Core::ConnectionState state);
    void setAutoConnect(int row, bool enabled);

private:
    Core::Session *session;

    QVector<Core::AccountInfo> accounts;
    mutable QMultiHash<QUrl, QPersistentModelIndex> pendingRequests;
};

} // namespace UI
} // namespace Acheron