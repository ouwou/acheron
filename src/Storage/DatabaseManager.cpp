#include "DatabaseManager.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Storage {

DatabaseManager &DatabaseManager::instance()
{
    static DatabaseManager instance;
    return instance;
}

bool DatabaseManager::init()
{
    QString dirPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(dirPath);
    if (!dir.exists())
        dir.mkpath(".");

    persistentPath = dir.filePath("acheron_persistent.sqlite");
    persistentDb = QSqlDatabase::addDatabase("QSQLITE", PERSISTENT_CONN_NAME);
    persistentDb.setDatabaseName(persistentPath);

    if (!persistentDb.open()) {
        qCCritical(LogDB) << "Persistent DB init failed:" << persistentDb.lastError().text();
        return false;
    }

    setupPersistentTables();
    return true;
}

void DatabaseManager::shutdown()
{
    if (persistentDb.isOpen())
        persistentDb.close();
    persistentDb = QSqlDatabase();
    QSqlDatabase::removeDatabase(PERSISTENT_CONN_NAME);
}

QString DatabaseManager::openCacheDatabase(Core::Snowflake accountId)
{
    QString connName = getCacheConnectionName(accountId);

    if (QSqlDatabase::contains(connName))
        return connName;

    QString dirPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString dbPath = QDir(dirPath).filePath(QString("cache_%1.sqlite").arg(accountId.toString()));

    if (QFile::exists(dbPath))
        QFile::remove(dbPath);

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
    db.setDatabaseName(dbPath);

    if (!db.open()) {
        qCCritical(LogDB) << "Cache DB init failed:" << db.lastError().text();
        return "";
    }

    QSqlQuery config(db);
    config.exec("PRAGMA journal_mode = WAL");
    config.exec("PRAGMA synchronous = OFF");

    setupCacheTables(connName);

    return connName;
}

void DatabaseManager::closeCacheDatabase(Core::Snowflake accountId)
{
    QString connName = getCacheConnectionName(accountId);

    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase db = QSqlDatabase::database(connName);
        if (db.isOpen())
            db.close();
        QSqlDatabase::removeDatabase(connName);
    }
}

QString DatabaseManager::getCacheConnectionName(Core::Snowflake accountId)
{
    return QString("Acheron_Cache_%1").arg(accountId.toString());
}

void DatabaseManager::setupPersistentTables()
{
    QSqlDatabase db = QSqlDatabase::database(PERSISTENT_CONN_NAME);
    QSqlQuery query(db);

    query.exec(R"(
        CREATE TABLE IF NOT EXISTS accounts (
            id INTEGER PRIMARY KEY,
            username TEXT,
            display_name TEXT,
            token TEXT
        )
    )");
}
void DatabaseManager::setupCacheTables(const QString &connName)
{
    QSqlDatabase db = QSqlDatabase::database(connName);
    QSqlQuery query(db);

    query.exec(R"(
        CREATE TABLE "guilds" (
	        "id" INTEGER NOT NULL,
	        "name" TEXT NOT NULL,
	        "icon" TEXT,
	        "owner_id" INTEGER NOT NULL,
	        PRIMARY KEY("id")
        )
    )");

    query.exec(R"(
        CREATE TABLE "channels" (
	        "id" INTEGER NOT NULL,
	        "type" INTEGER NOT NULL,
	        "position" INTEGER,
	        "name" INTEGER,
	        "guild_id" INTEGER,
	        "parent_id" INTEGER,
	        PRIMARY KEY("id")
        );
    )");

    query.exec(R"(
        CREATE TABLE "messages" (
	        "id" INTEGER NOT NULL,
	        "author_id" INTEGER NOT NULL,
	        "channel_id" INTEGER NOT NULL,
	        "content" TEXT NOT NULL,
	        "timestamp" TEXT NOT NULL,
	        "edited_timestamp" TEXT,
	        "type" INTEGER NOT NULL,
	        "flags" INTEGER NOT NULL,
	        PRIMARY KEY("id")
        );
    )");

    query.exec(R"(
        CREATE TABLE "users" (
	        "id" INTEGER NOT NULL,
	        "username" TEXT NOT NULL,
	        "global_name" TEXT,
	        "avatar" TEXT,
	        "bot" INTEGER,
            PRIMARY KEY("id")
        );
    )");
}
} // namespace Storage
} // namespace Acheron
