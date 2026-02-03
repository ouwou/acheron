#pragma once

#include <QObject>
#include <QString>

#include <curl/curl.h>

#include <memory>

#include "Enums.hpp"
#include "IngestThread.hpp"
#include "Inbound.hpp"
#include "Events.hpp"

namespace Acheron {

namespace Proto {
struct PreloadedUserSettings;
}

namespace Discord {

class ClientIdentity;

class Gateway : public QObject
{
    Q_OBJECT
public:
    explicit Gateway(const QString &token, const QString &gatewayUrl, ClientIdentity &identity,
                     QObject *parent = nullptr);
    ~Gateway();

    void start();
    void stop();
    void hardStop();

    void subscribeToGuild(Core::Snowflake guildId);
    void requestGuildMembers(Core::Snowflake guildId, const QList<Core::Snowflake> &userIds);

signals:
    void connected();
    void disconnected(CloseCode code, const QString &reason);

    void gatewayHello();
    void gatewayReady(const Ready &data);
    void gatewayReadySupplemental(const ReadySupplemental &data);
    void gatewayMessageCreate(const Message &data);
    void gatewayMessageUpdate(const Message &data);
    void gatewayMessageDelete(const MessageDelete &data);
    void gatewayTypingStart(const TypingStart &data);
    void gatewayChannelCreate(const ChannelCreate &data);
    void gatewayChannelUpdate(const ChannelUpdate &data);
    void gatewayChannelDelete(const ChannelDelete &data);
    void gatewayGuildMembersChunk(const GuildMembersChunk &data);

private:
    void sendPayload(const QJsonObject &obj);
    void sendPayload(const QByteArray &data);

    // this function is called by the network thread
    void onPayloadReceived(const QJsonObject &root);
    void handleDispatch(const Inbound &data);
    void handleReady(const Inbound &data);
    void handleReadySupplemental(const Inbound &data);
    void handleMessageCreate(const Inbound &data);
    void handleMessageUpdate(const Inbound &data);
    void handleMessageDelete(const Inbound &data);
    void handleTypingStart(const Inbound &data);
    void handleChannelCreate(const Inbound &data);
    void handleChannelUpdate(const Inbound &data);
    void handleChannelDelete(const Inbound &data);
    void handleGuildMembersChunk(const Inbound &data);
    void handleHello(const Inbound &data);
    void identify();

    void networkLoop();
    void heartbeatLoop();

private:
    QString token;
    QString gatewayUrl;
    ClientIdentity &identity;

    std::atomic<bool> running;

    std::mutex curlMutex;
    CURL *curl = nullptr;

    QByteArray receiveBuffer;
    IngestThread *ingest;

    bool wantToClose = false;
    std::thread networkThread;
    std::chrono::steady_clock::time_point closeTime;
    static constexpr std::chrono::milliseconds closeTimeout = std::chrono::milliseconds(1000);

    std::atomic<int> lastReceivedSequence = 0;

    std::atomic<int> heartbeatInterval = 0;
    std::mutex heartbeatMutex;
    std::condition_variable heartbeatCv;
    std::thread heartbeatThread;
};

} // namespace Discord
} // namespace Acheron
