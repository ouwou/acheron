#pragma once

#include <QObject>
#include <QTimer>

#include <functional>

#include "Core/Audio/VoiceConnection.hpp"
#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Streams/StreamManager.hpp"
#include "Discord/Voice/VoiceClient.hpp"

namespace Acheron {
namespace Core {

namespace Audio {
class VoiceManager;
}

class StreamConnection : public QObject
{
    Q_OBJECT
public:
    enum class Failure {
        TimedOut,
        Lost,
        ProxyRefused,
    };
    Q_ENUM(Failure)

    struct Account
    {
        Snowflake userId;
        QString gatewaySessionId;
        ProxyConfig proxy;

        [[nodiscard]] static Account of(const Audio::VoiceManager &voice);
    };

    using Configure = std::function<void(Discord::Voice::ConnectionParams &)>;

    StreamConnection(const Account &account, Configure configure, QObject *parent = nullptr);
    ~StreamConnection() override;

    void apply(const StreamRecord &record);
    // `beforeStop` runs on the connection's thread before its client stops
    void close(const std::function<void()> &beforeStop = {});

    [[nodiscard]] Audio::VoiceConnection *connection() const { return current; }
    [[nodiscard]] bool isConnected() const { return connectedNow; }

    template <typename Call>
    void run(Call call)
    {
        if (current)
            current->run(std::move(call));
    }

    static constexpr int CONNECT_TIMEOUT_MS = 12000;
    static constexpr int RECONNECT_DELAY_MS = 1000;
    static constexpr int MAX_RECONNECTS = 5;

signals:
    // not started yet, so its client can be wired up on its own thread first
    void opened(Acheron::Core::Audio::VoiceConnection *connection);
    void connected();
    void closed(Acheron::Core::Audio::VoiceConnection *connection);
    void reconnecting();
    void failed(Acheron::Core::StreamConnection::Failure failure);
    // as the official client does when its connection drops
    void pingRequested();

private slots:
    void onConnectTimedOut();
    void onReconnectDelayOver();

private:
    void connectNow();
    void onStateChanged(Discord::Voice::VoiceClient::State state);
    void onDisconnected();
    void dropConnection(const std::function<void()> &beforeStop);
    void fail(Failure failure);

    Account account;
    Configure configure;

    Snowflake rtcServerId;
    Snowflake rtcChannelId;
    QString endpoint;
    QString token;

    Audio::VoiceConnection *current = nullptr;
    bool connectedNow = false;
    bool failedNow = false;
    int reconnects = 0;
    QTimer connectTimeout;
    QTimer reconnectDelay;
};

} // namespace Core
} // namespace Acheron
