#pragma once

#include <QObject>
#include <QString>

#include <memory>

#include "AccountInfo.hpp"
#include "Discord/Client.hpp"
#include "MessageManager.hpp"

namespace Acheron {
namespace Core {

class ClientInstance : public QObject
{
    Q_OBJECT
public:
    explicit ClientInstance(const AccountInfo &info, QObject *parent = nullptr);
    ~ClientInstance() override;

    void start();
    void stop();

    [[nodiscard]] Discord::Client *discord() const;
    [[nodiscard]] MessageManager *messages() const;

    Snowflake accountId() const;
    const AccountInfo &accountInfo() const;

signals:
    void stateChanged(ConnectionState state);
    void detailsUpdated(const AccountInfo &info);

    void ready(const Discord::Ready &ready);

private slots:

private:
    AccountInfo account;

    MessageManager *messageManager;
    Discord::Client *client;
};

} // namespace Core
} // namespace Acheron