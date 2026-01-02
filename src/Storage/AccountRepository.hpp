#pragma once

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>

#include "DatabaseManager.hpp"
#include "Core/AccountInfo.hpp"

namespace Acheron {
namespace Storage {

class AccountRepository
{
public:
    AccountRepository() = default;

    void saveAccount(const Core::AccountInfo &acc);
    Core::AccountInfo getAccount(quint64 id);
    QVector<Core::AccountInfo> getAllAccounts();
    void removeAccount(quint64 id);
};

} // namespace Storage
} // namespace Acheron
