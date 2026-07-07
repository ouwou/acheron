#include "UserRepository.hpp"

#include "DatabaseManager.hpp"
#include "Transaction.hpp"
#include "Core/Logging.hpp"

namespace Acheron {
namespace Storage {

UserRepository::UserRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId))
{
}

void UserRepository::saveUser(const Discord::User &user)
{
    auto db = getDb();
    saveUser(user, db);
}

void UserRepository::saveUser(const Discord::User &user, QSqlDatabase &db)
{
    QSqlQuery q(db);
    q.prepare(R"(
        INSERT OR REPLACE INTO users
        (id, username, global_name, avatar, bot)
        VALUES (:id, :username, :global_name, :avatar, :bot)
    )");

    q.bindValue(":id", static_cast<qint64>(user.id.get()));
    q.bindValue(":username", user.username);
    bindOptional(q, ":global_name", user.globalName);
    bindOptional(q, ":avatar", user.avatar);
    bindOptional(q, ":bot", user.bot);

    execLogged(q, "UserRepository: Save user");
}

void UserRepository::saveUsers(const QList<Discord::User> &users)
{
    auto db = getDb();
    saveUsers(users, db);
}

void UserRepository::saveUsers(const QList<Discord::User> &users, QSqlDatabase &db)
{
    if (users.isEmpty())
        return;

    Transaction txn(db);
    for (const auto &user : users)
        saveUser(user, db);

    txn.commit();
}

std::optional<Discord::User> UserRepository::getUser(Core::Snowflake userId)
{
    auto db = getDb();
    QSqlQuery q(db);
    q.prepare(R"(
        SELECT id, username, global_name, avatar, bot
        FROM users WHERE id = :id
    )");
    q.bindValue(":id", static_cast<qint64>(userId));

    if (!q.exec() || !q.next())
        return std::nullopt;

    Discord::User user;
    user.id = static_cast<Core::Snowflake>(q.value(0).toLongLong());
    user.username = q.value(1).toString();
    if (!q.value(2).isNull())
        user.globalName = q.value(2).toString();
    if (!q.value(3).isNull())
        user.avatar = q.value(3).toString();
    if (!q.value(4).isNull())
        user.bot = q.value(4).toBool();

    return user;
}

} // namespace Storage
} // namespace Acheron
