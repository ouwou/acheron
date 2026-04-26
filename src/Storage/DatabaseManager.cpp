#include "DatabaseManager.hpp"

#include <QSettings>

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

    persistentPath = dir.filePath("acheron.sqlite");
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

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);

    bool inMemory = QSettings().value("general/in_memory_cache", false).toBool();
    if (inMemory) {
        db.setDatabaseName(":memory:");
    } else {
        QString dirPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QString dbPath = QDir(dirPath).filePath(QString("cache_%1.sqlite").arg(accountId.toString()));

        if (QFile::exists(dbPath))
            QFile::remove(dbPath);

        db.setDatabaseName(dbPath);
    }

    if (!db.open()) {
        qCCritical(LogDB) << "Cache DB init failed:" << db.lastError().text();
        return "";
    }

    if (!inMemory) {
        QSqlQuery config(db);
        config.exec("PRAGMA journal_mode = WAL");
        config.exec("PRAGMA synchronous = OFF");
    }

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
            avatar TEXT,
            gateway_url TEXT,
            rest_url TEXT,
            cdn_url TEXT,
            display_order INTEGER DEFAULT 0,
            auto_connect INTEGER DEFAULT 0
        )
    )");

    // Lightweight migration for existing installs: ensure auto_connect exists
    bool hasAutoConnect = false;
    QSqlQuery infoQuery(db);
    if (infoQuery.exec("PRAGMA table_info(accounts)")) {
        while (infoQuery.next()) {
            if (infoQuery.value("name").toString() == "auto_connect") {
                hasAutoConnect = true;
                break;
            }
        }
    }

    if (!hasAutoConnect) {
        if (!query.exec("ALTER TABLE accounts ADD COLUMN auto_connect INTEGER DEFAULT 0")) {
            qCWarning(LogDB) << "Failed to migrate accounts.auto_connect:"
                             << query.lastError().text();
        }
    }
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
	        "last_message_id" INTEGER,
	        "icon" TEXT,
	        "owner_id" INTEGER,
	        "rate_limit_per_user" INTEGER,
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
	        "embeds" TEXT,
	        "reactions" TEXT,
	        "deleted" INTEGER NOT NULL,
	        "referenced_message_id" INTEGER,
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

    query.exec(R"(
        CREATE TABLE "members" (
            "user_id" INTEGER NOT NULL,
            "guild_id" INTEGER NOT NULL,
            "nick" TEXT,
            "avatar" TEXT,
            "roles" TEXT,
            "joined_at" TEXT,
            "premium_since" TEXT,
            "deaf" INTEGER,
            "mute" INTEGER,
            "flags" INTEGER,
            "pending" INTEGER,
            "communication_disabled_until" TEXT,
            PRIMARY KEY("user_id", "guild_id")
        );
    )");

    query.exec(R"(
        CREATE TABLE "attachments" (
            "id" INTEGER NOT NULL,
            "message_id" INTEGER NOT NULL,
            "filename" TEXT NOT NULL,
            "content_type" TEXT,
            "size" INTEGER NOT NULL,
            "url" TEXT NOT NULL,
            "proxy_url" TEXT NOT NULL,
            "width" INTEGER,
            "height" INTEGER,
            PRIMARY KEY("id")
        );
    )");

    query.exec("CREATE INDEX idx_attachments_message_id ON attachments(message_id);");

    query.exec(R"(
        CREATE TABLE "roles" (
            "id" INTEGER NOT NULL,
            "guild_id" INTEGER NOT NULL,
            "name" TEXT NOT NULL,
            "permissions" INTEGER NOT NULL,
            "position" INTEGER NOT NULL,
            "color" INTEGER,
            "hoist" INTEGER,
            "icon" TEXT,
            "unicode_emoji" TEXT,
            "managed" INTEGER,
            "mentionable" INTEGER,
            PRIMARY KEY("id", "guild_id"),
            FOREIGN KEY("guild_id") REFERENCES guilds("id") ON DELETE CASCADE
        );
    )");

    query.exec("CREATE INDEX idx_roles_guild_id ON roles(guild_id);");
    query.exec("CREATE INDEX idx_roles_position ON roles(guild_id, position);");
    query.exec("CREATE INDEX idx_roles_lookup ON roles(guild_id, id);");

    query.exec(R"(
        CREATE TABLE "permission_overwrites" (
            "channel_id" INTEGER NOT NULL,
            "target_id" INTEGER NOT NULL,
            "type" INTEGER NOT NULL,
            "allow" INTEGER NOT NULL,
            "deny" INTEGER NOT NULL,
            PRIMARY KEY("channel_id", "target_id"),
            FOREIGN KEY("channel_id") REFERENCES channels("id") ON DELETE CASCADE
        );
    )");

    query.exec("CREATE INDEX idx_overwrites_channel_id ON permission_overwrites(channel_id);");
    query.exec("CREATE INDEX idx_members_lookup ON members(guild_id, user_id);");
    query.exec("CREATE INDEX idx_channels_guild ON channels(guild_id);");

    query.exec(R"(
        CREATE TABLE "channel_recipients" (
            "channel_id" INTEGER NOT NULL,
            "user_id" INTEGER NOT NULL,
            PRIMARY KEY("channel_id", "user_id"),
            FOREIGN KEY("channel_id") REFERENCES channels("id") ON DELETE CASCADE,
            FOREIGN KEY("user_id") REFERENCES users("id")
        );
    )");

    query.exec("CREATE INDEX idx_channel_recipients_channel ON channel_recipients(channel_id);");
}
} // namespace Storage
} // namespace Acheron
