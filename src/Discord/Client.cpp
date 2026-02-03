#include "Client.hpp"

#include <QDebug>
#include <QJsonObject>

#include "Enums.hpp"
#include "Core/Logging.hpp"
#include "Proto/ProtoReader.hpp"
#include "Proto/UserSettings.hpp"

namespace Acheron {
namespace Discord {

Client::Client(const QString &token, const QString &gatewayUrl, const QString &baseUrl,
               QObject *parent)
    : QObject(parent), token(token), baseUrl(baseUrl)
{
    identity.regenerateClientHeartbeatSessionId();

    gateway = new Gateway(token, gatewayUrl, identity, this);
    httpClient = new HttpClient(baseUrl, token, identity, this);

    connect(gateway, &Gateway::connected, this, &Client::onConnected);
    connect(gateway, &Gateway::disconnected, this, &Client::onDisconnected);

    connect(gateway, &Gateway::gatewayReady, this, &Client::onGatewayReady);
    connect(gateway, &Gateway::gatewayReadySupplemental, this, &Client::onGatewayReadySupplemental);
    connect(gateway, &Gateway::gatewayMessageCreate, this, &Client::onGatewayMessageCreate);
    connect(gateway, &Gateway::gatewayMessageUpdate, this, &Client::onGatewayMessageUpdate);
    connect(gateway, &Gateway::gatewayMessageDelete, this, &Client::onGatewayMessageDelete);
    connect(gateway, &Gateway::gatewayTypingStart, this, &Client::typingStart);
    connect(gateway, &Gateway::gatewayChannelCreate, this, &Client::onGatewayChannelCreate);
    connect(gateway, &Gateway::gatewayChannelUpdate, this, &Client::onGatewayChannelUpdate);
    connect(gateway, &Gateway::gatewayChannelDelete, this, &Client::onGatewayChannelDelete);
    connect(gateway, &Gateway::gatewayGuildMembersChunk, this, &Client::guildMembersChunk);
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

    const QByteArray binary = QByteArray::fromBase64(data.userSettingsProto->toUtf8());
    Proto::ProtoReader reader(binary);
    settings = Proto::PreloadedUserSettings::fromProto(reader);
    me = data.user;

    emit ready(data);
}

void Client::onGatewayReadySupplemental(const ReadySupplemental &data)
{
    emit readySupplemental(data);
}

void Client::onGatewayMessageCreate(const Message &msg)
{
    emit messageCreated(msg);
}

void Client::onGatewayMessageUpdate(const Message &msg)
{
    emit messageUpdated(msg);
}

void Client::onGatewayMessageDelete(const MessageDelete &event)
{
    emit messageDeleted(event);
}

void Client::onGatewayChannelCreate(const ChannelCreate &event)
{
    emit channelCreated(event);
}

void Client::onGatewayChannelUpdate(const ChannelUpdate &event)
{
    emit channelUpdated(event);
}

void Client::onGatewayChannelDelete(const ChannelDelete &event)
{
    emit channelDeleted(event);
}

void Client::sendMessage(Snowflake channelId, const QString &content, const QString &nonce)
{
    QString endpoint = "/channels/" + QString::number(channelId) + "/messages";

    // todo extract to struct probably
    QJsonObject payload;
    payload["content"] = content;
    payload["flags"] = 0;
    payload["mobile_network_type"] = "unknown";
    payload["nonce"] = nonce;
    payload["tts"] = false;

    httpClient->post(endpoint, payload, [this, channelId, nonce](const HttpResponse &response) {
        if (!response.success) {
            qCWarning(LogDiscord) << "Failed to send message:" << response.error
                                  << "Status:" << response.statusCode;
            emit messageSendFailed(nonce, response.error);
            return;
        }

        qCInfo(LogDiscord) << "Message sent successfully to channel" << channelId;
    });
}

void Client::ensureSubscriptionByChannel(Snowflake channelId)
{
    if (!channelToGuild.contains(channelId))
        return;

    Snowflake guildId = channelToGuild.value(channelId);

    if (!subscribedGuilds.contains(guildId)) {
        gateway->subscribeToGuild(guildId);
    }
}

void Client::requestGuildMembers(Snowflake guildId, const QList<Snowflake> &userIds)
{
    gateway->requestGuildMembers(guildId, userIds);
}

[[nodiscard]] const Proto::PreloadedUserSettings &Client::getSettings() const
{
    return settings;
}

[[nodiscard]] const User &Client::getMe() const
{
    return me;
}

void Client::setState(Core::ConnectionState state)
{
    if (this->state != state) {
        this->state = state;
        emit stateChanged(state);
    }
}

} // namespace Discord
} // namespace Acheron