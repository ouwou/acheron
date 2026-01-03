#include "ClientInstance.hpp"

#include "Storage/DatabaseManager.hpp"
#include "Storage/GuildRepository.hpp"
#include "Storage/ChannelRepository.hpp"

namespace Acheron {
namespace Core {
ClientInstance::ClientInstance(const AccountInfo &info, QObject *parent)
    : QObject(parent), account(info)
{
    client = new Discord::Client(info.token, info.gatewayUrl, info.restUrl, this);
    messageManager = new MessageManager(info.id, client, this);

    Storage::DatabaseManager::instance().openCacheDatabase(info.id);

    connect(client, &Discord::Client::stateChanged, this, &ClientInstance::stateChanged);

    connect(client, &Discord::Client::ready, this, [this](const Discord::Ready &ready) {
        QString connName = Storage::DatabaseManager::instance().getCacheConnectionName(account.id);
        QSqlDatabase db = QSqlDatabase::database(connName);

        Storage::GuildRepository guildRepo(account.id);
        Storage::ChannelRepository channelRepo(account.id);

        account.username = ready.user->username;
        account.displayName = ready.user->globalName;
        account.avatar = ready.user->avatar;

        db.transaction();
        for (const auto &guild : ready.guilds.get()) {
            guildRepo.saveGuild(guild.asGuild(), db);
            for (const auto &channel : guild.channels.get()) {
                // we dont get a guild_id from the gateway here
                Discord::Channel copy = channel;
                copy.guildId = guild.properties->id;
                channelRepo.saveChannel(copy, db);
            }
        }
        db.commit();

        emit detailsUpdated(account);
        emit this->ready(ready);
    });

    connect(client, &Discord::Client::messageCreated, messageManager,
            &MessageManager::onMessageCreated);
}

ClientInstance::~ClientInstance()
{
    Storage::DatabaseManager::instance().closeCacheDatabase(account.id);
}

void ClientInstance::start()
{
    discord()->start();
}

void ClientInstance::stop()
{
    discord()->stop();
}

Discord::Client *ClientInstance::discord() const
{
    return client;
}

MessageManager *ClientInstance::messages() const
{
    return messageManager;
}

Snowflake ClientInstance::accountId() const
{
    return account.id;
}

const AccountInfo &ClientInstance::accountInfo() const
{
    return account;
}
} // namespace Core
} // namespace Acheron
