#include "Session.hpp"

#include <QDebug>

#include "Logging.hpp"

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
        qCWarning(LogCore) << "Account already connected or connecting:" << accountId;
        return;
    }

    AccountInfo acc = repo.getAccount(accountId);
    if (acc.id == 0 || acc.token.isEmpty()) {
        qCWarning(LogCore) << "Account not found or invalid token:" << accountId;
        return;
    }

    auto *instance = new ClientInstance(acc, this);

    connect(instance, &ClientInstance::stateChanged, this,
            [this, accountId](ConnectionState state) {
                emit connectionStateChanged(accountId, state);
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

    connect(instance->discord(), &Discord::Client::stateChanged, instance,
            [instance](ConnectionState state) {
                if (state == ConnectionState::Disconnected)
                    instance->deleteLater();
            });
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

} // namespace Core
} // namespace Acheron