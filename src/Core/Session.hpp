#pragma once

#include <QObject>
#include <QMap>
#include <memory>

#include "ClientInstance.hpp"
#include "Storage/AccountRepository.hpp"
#include "Core/Enums.hpp"
#include "Core/ImageManager.hpp"

namespace Acheron {
namespace Core {

class Session : public QObject
{
    Q_OBJECT
public:
    explicit Session(QObject *parent = nullptr);
    ~Session() override;

    void start();
    void shutdown();

    void connectAccount(Snowflake accountId);
    void disconnectAccount(Snowflake accountId);

    [[nodiscard]] QList<ClientInstance *> getClients() const { return clients.values(); }
    [[nodiscard]] ClientInstance *client(Snowflake accountId) const;
    [[nodiscard]] AccountInfo getAccountInfo(Snowflake accountId);
    [[nodiscard]] ImageManager *getImageManager() { return imageManager; }
    [[nodiscard]] bool hasActiveConnection() const;

signals:
    void connectionStateChanged(Snowflake accountId, Core::ConnectionState newState);
    void accountDetailsUpdated(const Core::AccountInfo &info);

    void ready(const Discord::Ready &ready);

private:
    ImageManager *imageManager;
    Storage::AccountRepository repo;
    QMap<Snowflake, ClientInstance *> clients;
};

} // namespace Core
} // namespace Acheron