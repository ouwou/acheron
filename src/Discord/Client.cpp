#include "Client.hpp"

#include <QDebug>

#include "Enums.hpp"
#include "Core/Logging.hpp"

namespace Acheron {
namespace Discord {

Client::Client(const QString &token, const QString &gatewayUrl, const QString &baseUrl,
               QObject *parent)
    : QObject(parent), m_token(token), baseUrl(baseUrl)
{
    netManager = new QNetworkAccessManager(this);

    gateway = new Gateway(token, gatewayUrl, this);
    httpClient = new HttpClient(baseUrl, token, this);

    connect(gateway, &Gateway::connected, this, &Client::onConnected);
    connect(gateway, &Gateway::disconnected, this, &Client::onDisconnected);

    connect(gateway, &Gateway::gatewayReady, this, &Client::onGatewayReady);
    connect(gateway, &Gateway::gatewayMessageCreate, this, &Client::onGatewayMessageCreate);
}

void Client::start()
{
    setState(Core::ConnectionState::Connecting);
    gateway->start();
}

void Client::stop()
{
    setState(Core::ConnectionState::Disconnecting);
    gateway->stop();
}

[[nodiscard]] Core::ConnectionState Client::getState() const
{
    return state;
}

void Client::fetchLatestMessages(Snowflake channelId, int limit, MessagesCallback callback)
{
    QString endpoint = "/channels/" + QString::number(channelId) + "/messages";
    QUrlQuery query;
    query.addQueryItem("limit", QString::number(limit));

    httpClient->get(endpoint, query, [this, channelId, callback](const HttpResponse &response) {
        if (!response.success) {
            qCWarning(LogDiscord) << "Failed to fetch latest messages: " << response.error;
            callback({ {}, "Failed to fetch latest messages: " + response.error });
            return;
        }

        QList<Message> results;
        QJsonArray arr = QJsonDocument::fromJson(response.body).array();
        for (const QJsonValue &val : arr)
            results.append(Message::fromJson(val.toObject()));

        callback({ results });
    });
}

void Client::fetchHistory(Snowflake channelId, Snowflake beforeId, int limit,
                          MessagesCallback callback)
{
    QString endpoint = "/channels/" + QString::number(channelId) + "/messages";
    QUrlQuery query;
    query.addQueryItem("before", QString::number(beforeId));
    query.addQueryItem("limit", QString::number(limit));

    httpClient->get(endpoint, query, [this, channelId, callback](const HttpResponse &response) {
        if (!response.success) {
            qCWarning(LogDiscord) << "Failed to fetch history: " << response.error;
            callback({ {}, "Failed to fetch history: " + response.error });
            return;
        }

        QList<Message> results;
        QJsonArray arr = QJsonDocument::fromJson(response.body).array();
        for (const QJsonValue &val : arr)
            results.append(Message::fromJson(val.toObject()));

        callback({ results });
    });
}

void Client::onConnected()
{
    qInfo() << "Connected to gateway";

    setState(Core::ConnectionState::Connected);
}

void Client::onDisconnected(CloseCode code, const QString &reason)
{
    qWarning() << "Disconnected from gateway: " << code << reason;

    setState(Core::ConnectionState::Disconnected);

    if (code == CloseCode::AUTHENTICATION_FAILED) {
        emit errorOccurred("Invalid token");
        return;
    } else {
        emit errorOccurred("Disconnected from gateway");
        return;
    }
}

void Client::onGatewayReady(const Ready &data)
{
    for (const auto &guild : data.guilds.get()) {
        for (const auto &channel : guild.channels.get()) {
            channelToGuild.insert(channel.id, guild.properties->id.get());
        }
    }
    emit ready(data);
}

void Client::onGatewayMessageCreate(const Message &msg)
{
    emit messageCreated(msg);
}

void Client::postMessage(Snowflake channelId, const QString &content) { }

void Client::ensureSubscriptionByChannel(Snowflake channelId)
{
    if (!channelToGuild.contains(channelId))
        return;

    Snowflake guildId = channelToGuild.value(channelId);

    if (!subscribedGuilds.contains(guildId)) {
        gateway->subscribeToGuild(guildId);
    }
}

void Client::setState(Core::ConnectionState state)
{
    if (this->state != state) {
        this->state = state;
        emit stateChanged(state);
    }
}

void Client::sendRequest(const QString &endpoint, const QUrlQuery &query,
                         const std::function<void(QNetworkReply *)> &callback)
{
    QUrl url(baseUrl + endpoint);
    url.setQuery(query);
    QNetworkRequest request(url);

    if (!m_token.isEmpty()) {
        request.setRawHeader("Authorization", m_token.toUtf8());
    }

    QNetworkReply *reply = netManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "Network Error:" << reply->errorString();
            emit errorOccurred(reply->errorString());
            return;
        }

        callback(reply);
    });
}

} // namespace Discord
} // namespace Acheron