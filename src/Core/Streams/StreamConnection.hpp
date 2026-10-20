#pragma once

#include <QObject>
#include <QTimer>

#include <functional>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Streams/StreamManager.hpp"
#include "Discord/Voice/VoiceClient.hpp"

namespace Acheron {
namespace Core {

namespace Audio {
class VoiceConnection;
}

// The RTC connection of one Go Live stream, for its owner or a viewer. It is made where the
// gateway says the stream is hosted, made again there when it drops, as often as MAX_RECONNECTS
// times in a row and each time after a ping that keeps the stream for this account, and given
// up on when it is neither up nor failed CONNECT_TIMEOUT_MS after it was asked for.
class StreamConnection : public QObject
{
    Q_OBJECT
public:
    enum class Failure {
        // Not up in time, from the first ask or after a drop.
        TimedOut,
        // Dropped more often in a row than it is made again.
        Lost,
        // The account's proxy cannot carry what an RTC connection sends.
        ProxyRefused,
    };
    Q_ENUM(Failure)

    struct Account
    {
        Snowflake userId;
        QString gatewaySessionId;
        ProxyConfig proxy;
    };

    // Fills in what the owner or the viewer has to say of each connection before it is made:
    // the codecs it offers, and whether it sends sound, which none does unless it is said here.
    using Configure = std::function<void(Discord::Voice::ConnectionParams &)>;

    StreamConnection(const Account &account, Configure configure, QObject *parent = nullptr);
    ~StreamConnection() override;

    // What the gateway now says about where the stream is hosted.
    void apply(const StreamRecord &record);
    // `beforeStop` runs on the connection's thread ahead of its client being stopped.
    void close(const std::function<void()> &beforeStop = {});

    // Null between connections.
    [[nodiscard]] Audio::VoiceConnection *connection() const { return current; }
    [[nodiscard]] bool isConnected() const { return connectedNow; }

    static constexpr int CONNECT_TIMEOUT_MS = 12000;
    static constexpr int RECONNECT_DELAY_MS = 1000;
    static constexpr int MAX_RECONNECTS = 5;

signals:
    // A connection is made and not started yet: what has to be wired to its client, on its
    // thread, is wired now.
    void opened(Acheron::Core::Audio::VoiceConnection *connection);
    // Up, the first time and again after the client resumed a dropped connection by itself.
    void connected();
    // A connection has stopped, and nothing more comes from it. Its client is still there,
    // for whatever held on to it to let go of.
    void closed(Acheron::Core::Audio::VoiceConnection *connection);
    // It dropped or the stream moved, and another is on its way.
    void reconnecting();
    void failed(Acheron::Core::StreamConnection::Failure failure);
    // The gateway is to be told the stream is still wanted, as the official client does when
    // its connection drops.
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

    // Where the gateway last said the stream is hosted.
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
