#include "AccountRepository.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Storage {

void AccountRepository::saveAccount(const Core::AccountInfo &acc)
{
    QSqlDatabase db = QSqlDatabase::database(DatabaseManager::PERSISTENT_CONN_NAME);
    if (!db.isOpen()) {
        qCWarning(LogDB) << "AccountRepository: Persistent DB not open!";
        return;
    }

    QSqlQuery query(db);
    query.prepare(R"(
            INSERT OR REPLACE INTO accounts 
            (id, username, display_name, token)
            VALUES (:id, :username, :display_name, :token)
        )");

    query.bindValue(":id", static_cast<qint64>(acc.id));
    query.bindValue(":username", acc.username);
    query.bindValue(":display_name", acc.displayName);
    query.bindValue(":token", acc.token);

    if (!query.exec())
        qCWarning(LogDB) << "AccountRepository: Save failed:" << query.lastError().text();
}

Core::AccountInfo AccountRepository::getAccount(quint64 id)
{
    QSqlDatabase db = QSqlDatabase::database(DatabaseManager::PERSISTENT_CONN_NAME);
    QSqlQuery query(db);

    query.prepare("SELECT * FROM accounts WHERE id = :id");

    query.bindValue(":id", static_cast<qint64>(id));

    if (!query.exec()) {
        qCWarning(LogDB) << "AccountRepository: Get failed:" << query.lastError().text();
        return {};
    }

    if (!query.next()) {
        qCWarning(LogDB) << "AccountRepository: Account not found";
        return {};
    }

    Core::AccountInfo acc;

    acc.id = static_cast<Core::Snowflake>(query.value("id").toLongLong());
    acc.username = query.value("username").toString();
    acc.displayName = query.value("display_name").toString();
    acc.token = query.value("token").toString();
    // acc.avatar =

    return acc;
}

QVector<Core::AccountInfo> AccountRepository::getAllAccounts()
{
    QVector<Core::AccountInfo> results;
    QSqlDatabase db = QSqlDatabase::database(DatabaseManager::PERSISTENT_CONN_NAME);

    if (!db.isOpen())
        return results;

    QSqlQuery query("SELECT * FROM accounts", db);

    while (query.next()) {
        Core::AccountInfo acc;

        acc.id = static_cast<quint64>(query.value("id").toLongLong());

        acc.username = query.value("username").toString();
        acc.displayName = query.value("display_name").toString();
        acc.token = query.value("token").toString();

        acc.state = Core::ConnectionState::Disconnected;

        // acc.avatar = defAv;

        results.append(acc);
    }

    return results;
}

void AccountRepository::removeAccount(quint64 id)
{
    QSqlDatabase db = QSqlDatabase::database(DatabaseManager::PERSISTENT_CONN_NAME);
    QSqlQuery query(db);

    query.prepare("DELETE FROM accounts WHERE id = :id");
    query.bindValue(":id", static_cast<qint64>(id));

    if (!query.exec()) {
        qCWarning(LogDB) << "AccountRepository: Remove failed:" << query.lastError().text();
    }
}

} // namespace Storage
} // namespace Acheron
