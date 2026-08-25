#pragma once

#include <QObject>
#include <QMap>
#include <QSet>
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

    void autoConnectAccounts();

    void setAccountProxy(Snowflake accountId, const ProxyConfig &proxy);

    [[nodiscard]] QList<ClientInstance *> getClients() const { return clients.values(); }
    [[nodiscard]] ClientInstance *client(Snowflake accountId) const;
    [[nodiscard]] AccountInfo getAccountInfo(Snowflake accountId);
    [[nodiscard]] ImageManager *getImageManager() { return imageManager; }
    [[nodiscard]] bool hasActiveConnection() const;

    void setCaptchaResolver(Discord::CaptchaResolver *resolver) { captchaResolver = resolver; }
    [[nodiscard]] Discord::CaptchaResolver *getCaptchaResolver() const { return captchaResolver; }

signals:
    void connectionStateChanged(Snowflake accountId, Core::ConnectionState newState);
    void accountDetailsUpdated(const Core::AccountInfo &info);

    void ready(const Discord::Ready &ready);

private:
    void startInstance(const AccountInfo &acc);

    ImageManager *imageManager;
    Storage::AccountRepository repo;
    QMap<Snowflake, ClientInstance *> clients;
    QSet<Snowflake> connectingAccounts;
    Discord::CaptchaResolver *captchaResolver = nullptr;
};

} // namespace Core
} // namespace Acheron