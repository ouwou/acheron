#include "AccountRepository.hpp"

#include "Core/Logging.hpp"
#include "Core/TokenStore.hpp"

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
            (id, username, display_name, avatar, gateway_url, rest_url, cdn_url, display_order, auto_connect)
            VALUES (:id, :username, :display_name, :avatar, :gateway_url, :rest_url, :cdn_url, :display_order, :auto_connect)
        )");

    query.bindValue(":id", static_cast<qint64>(acc.id));
    query.bindValue(":username", acc.username);
    query.bindValue(":display_name", acc.displayName);
    query.bindValue(":avatar", acc.avatar);
    query.bindValue(":gateway_url", acc.gatewayUrl);
    query.bindValue(":rest_url", acc.restUrl);
    query.bindValue(":cdn_url", acc.cdnUrl);
    query.bindValue(":display_order", acc.displayOrder);
    query.bindValue(":auto_connect", acc.autoConnect ? 1 : 0);

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
    acc.avatar = query.value("avatar").toString();
    acc.gatewayUrl = query.value("gateway_url").toString();
    acc.restUrl = query.value("rest_url").toString();
    acc.cdnUrl = query.value("cdn_url").toString();
    acc.displayOrder = query.value("display_order").toInt();
    acc.autoConnect = query.value("auto_connect").toInt() != 0;

    return acc;
}

QVector<Core::AccountInfo> AccountRepository::getAllAccounts()
{
    QVector<Core::AccountInfo> results;
    QSqlDatabase db = QSqlDatabase::database(DatabaseManager::PERSISTENT_CONN_NAME);

    if (!db.isOpen())
        return results;

    QSqlQuery query("SELECT * FROM accounts ORDER BY display_order ASC", db);

    while (query.next()) {
        Core::AccountInfo acc;

        acc.id = static_cast<quint64>(query.value("id").toLongLong());

        acc.username = query.value("username").toString();
        acc.displayName = query.value("display_name").toString();
        acc.avatar = query.value("avatar").toString();
        acc.gatewayUrl = query.value("gateway_url").toString();
        acc.restUrl = query.value("rest_url").toString();
        acc.cdnUrl = query.value("cdn_url").toString();
        acc.displayOrder = query.value("display_order").toInt();
        acc.autoConnect = query.value("auto_connect").toInt() != 0;

        acc.state = Core::ConnectionState::Disconnected;

        results.append(acc);
    }

    return results;
}

void AccountRepository::removeAccount(quint64 id)
{
    Core::TokenStore::deleteToken(Core::Snowflake(id));

    QSqlDatabase db = QSqlDatabase::database(DatabaseManager::PERSISTENT_CONN_NAME);
    QSqlQuery query(db);

    query.prepare("DELETE FROM accounts WHERE id = :id");
    query.bindValue(":id", static_cast<qint64>(id));

    if (!query.exec())
        qCWarning(LogDB) << "AccountRepository: Remove failed:" << query.lastError().text();
}

void AccountRepository::updateDisplayOrder(quint64 id, int order)
{
    QSqlDatabase db = QSqlDatabase::database(DatabaseManager::PERSISTENT_CONN_NAME);
    QSqlQuery query(db);

    query.prepare("UPDATE accounts SET display_order = :order WHERE id = :id");
    query.bindValue(":order", order);
    query.bindValue(":id", static_cast<qint64>(id));

    if (!query.exec())
        qCWarning(LogDB) << "AccountRepository: Update display order failed:"
                         << query.lastError().text();
}

void AccountRepository::updateAutoConnect(quint64 id, bool enabled)
{
    QSqlDatabase db = QSqlDatabase::database(DatabaseManager::PERSISTENT_CONN_NAME);
    QSqlQuery query(db);

    query.prepare("UPDATE accounts SET auto_connect = :enabled WHERE id = :id");
    query.bindValue(":enabled", enabled ? 1 : 0);
    query.bindValue(":id", static_cast<qint64>(id));

    if (!query.exec())
        qCWarning(LogDB) << "AccountRepository: Update auto connect failed:"
                         << query.lastError().text();
}

} // namespace Storage
} // namespace Acheron
