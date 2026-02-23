#include "Session.hpp"

#include <QDebug>

#include "Logging.hpp"
#include "TokenStore.hpp"

namespace Acheron {
namespace Core {

Session::Session(QObject *parent) : QObject(parent)
{
    imageManager = new ImageManager(this);
}

Session::~Session()
{
    shutdown();
}

void Session::start()
{
    qCDebug(LogCore) << "Session started";
}

void Session::shutdown()
{
    qCDebug(LogCore) << "Session shutdown";
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        it.value()->stop();
    }
    qDeleteAll(clients);
    clients.clear();
}

void Session::connectAccount(Snowflake accountId)
{
    if (clients.contains(accountId)) {
        ClientInstance *existing = clients.value(accountId);

        if (existing->state() != ConnectionState::Disconnected) {
            qCWarning(LogCore) << "Account already connected or connecting:" << accountId;
            return;
        }

        // were dead
        clients.take(accountId)->deleteLater();
    }

    AccountInfo acc = repo.getAccount(accountId);
    if (acc.id == 0) {
        qCWarning(LogCore) << "Account not found:" << accountId;
        return;
    }

    acc.token = TokenStore::loadToken(accountId);
    if (acc.token.isEmpty()) {
        qCWarning(LogCore) << "No token found in keychain for account:" << accountId;
        return;
    }

    auto *instance = new ClientInstance(acc, this);

    connect(instance, &ClientInstance::stateChanged, this,
            [this, accountId, instance](ConnectionState state) {
                emit connectionStateChanged(accountId, state);

                if (state == ConnectionState::Disconnected) {
                    if (clients.value(accountId) == instance) {
                        clients.take(accountId)->deleteLater();
                    }
                }
            });

    connect(instance, &ClientInstance::detailsUpdated, this, [this](const AccountInfo &info) {
        repo.saveAccount(info);
        emit accountDetailsUpdated(info);
    });

    connect(instance, &ClientInstance::ready, this,
            [this](const Discord::Ready &ready) { emit this->ready(ready); });

    clients.insert(accountId, instance);

    instance->start();
}

void Session::disconnectAccount(Snowflake accountId)
{
    if (!clients.contains(accountId))
        return;

    ClientInstance *instance = clients.take(accountId);

    instance->stop();
}

ClientInstance *Session::client(Snowflake accountId) const
{
    return clients.value(accountId, nullptr);
}

AccountInfo Session::getAccountInfo(Snowflake accountId)
{
    if (clients.contains(accountId)) {
        return clients[accountId]->accountInfo();
    }
    return repo.getAccount(accountId);
}

bool Session::hasActiveConnection() const
{
    for (auto *c : clients)
        if (c->state() == ConnectionState::Connected || c->state() == ConnectionState::Connecting)
            return true;
    return false;
}

} // namespace Core
} // namespace Acheron