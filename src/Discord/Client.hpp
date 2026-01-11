#pragma once

#include <QObject>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrlQuery>

#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Enums.hpp"

#include "Proto/UserSettings.hpp"

#include "Gateway.hpp"
#include "HttpClient.hpp"
#include "ClientIdentity.hpp"

using Acheron::Core::Snowflake;

namespace Acheron {
namespace Discord {

class Client : public QObject
{
    Q_OBJECT
public:
    enum class MessageLoadType {
        Latest,
        History,
        Future,
        Jump,
        Created,
    };
    Q_ENUM(MessageLoadType)

    explicit Client(const QString &token, const QString &gatewayUrl, const QString &baseUrl,
                    QObject *parent = nullptr);

    void start();
    void stop();

    [[nodiscard]] Core::ConnectionState getState() const;

    using MessagesCallback = std::function<void(const Core::Result<QList<Message>> &)>;
    void fetchLatestMessages(Snowflake channelId, int limit, MessagesCallback callback);
    void fetchHistory(Snowflake channelId, Snowflake beforeId, int limit,
                      MessagesCallback callback);
    // void fetchFuture(Snowflake channelId, Snowflake afterId, int limit = 50);
    // void fetchAround(Snowflake channelId, Snowflake targetId, int limit = 50);

    void postMessage(Snowflake channelId, const QString &content);

    void ensureSubscriptionByChannel(Snowflake guildId);

    [[nodiscard]] const Proto::PreloadedUserSettings &getSettings() const;

signals:
    void stateChanged(Core::ConnectionState state);
    void ready(const Ready &data);
    void messageCreated(const Message &msg);
    void typingStart(const TypingStart &event);

    void errorOccurred(const QString &errorStr);

private slots:
    void onConnected();
    void onDisconnected(CloseCode code, const QString &reason);

    void onGatewayReady(const Ready &data);
    void onGatewayMessageCreate(const Message &msg);

private:
    void setState(Core::ConnectionState state);

private:
    Core::ConnectionState state = Core::ConnectionState::Disconnected;

    QString baseUrl;
    QString token;

    ClientIdentity identity;
    HttpClient *httpClient;
    Gateway *gateway;

    QHash<Snowflake, Snowflake> channelToGuild; // todo prob move this somewhere or just a cache
    QSet<Snowflake> subscribedGuilds;

    Proto::PreloadedUserSettings settings;
};

} // namespace Discord
} // namespace Acheron