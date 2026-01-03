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
namespace Discord {

class Gateway : public QObject
{
    Q_OBJECT
public:
    explicit Gateway(const QString &token, const QString &gatewayUrl, QObject *parent = nullptr);
    ~Gateway();

    void start();
    void stop();
    void hardStop();

    void subscribeToGuild(Core::Snowflake guildId);

signals:
    void connected();
    void disconnected(CloseCode code, const QString &reason);

    void gatewayHello();
    void gatewayReady(const Ready &data);
    void gatewayMessageCreate(const Message &data);

private:
    void sendPayload(const QJsonObject &obj);
    void sendPayload(const QByteArray &data);

    // this function is called by the network thread
    void onPayloadReceived(const QJsonObject &root);
    void handleDispatch(const Inbound &data);
    void handleReady(const Inbound &data);
    void handleReadySupplemental(const Inbound &data);
    void handleMessageCreate(const Inbound &data);
    void handleHello(const Inbound &data);
    void identify();

    QString generateLaunchSignature();

    void networkLoop();
    void heartbeatLoop();

private:
    QString token;
    QString gatewayUrl;

    std::atomic<bool> running;

    std::mutex curlMutex;
    CURL *curl = nullptr;

    QByteArray receiveBuffer;
    IngestThread *ingest;

    bool wantToClose = false;
    std::thread networkThread;
    std::chrono::steady_clock::time_point closeTime;
    static constexpr std::chrono::milliseconds closeTimeout = std::chrono::milliseconds(1000);

    QString launchId;
    QString launchSignature;

    std::atomic<int> lastReceivedSequence = 0;

    std::atomic<int> heartbeatInterval = 0;
    std::mutex heartbeatMutex;
    std::condition_variable heartbeatCv;
    std::thread heartbeatThread;
};

} // namespace Discord
} // namespace Acheron
