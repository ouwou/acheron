#include "StreamConnection.hpp"

#include "Core/Audio/VoiceConnection.hpp"
#include "Core/Logging.hpp"

namespace Acheron {
namespace Core {

using Discord::Voice::VoiceClient;

namespace {

// A stream's RTC channel is the id one below its RTC server when the gateway names none.
Snowflake streamRtcChannelId(Snowflake rtcServerId, Snowflake rtcChannelId)
{
    return rtcChannelId.isValid() ? rtcChannelId : Snowflake(static_cast<quint64>(rtcServerId) - 1);
}

} // namespace

StreamConnection::StreamConnection(const Account &account, Configure configure, QObject *parent)
    : QObject(parent), account(account), configure(std::move(configure))
{
    connectTimeout.setSingleShot(true);
    connectTimeout.setInterval(CONNECT_TIMEOUT_MS);
    connect(&connectTimeout, &QTimer::timeout, this, &StreamConnection::onConnectTimedOut);
    connectTimeout.start();

    reconnectDelay.setSingleShot(true);
    reconnectDelay.setInterval(RECONNECT_DELAY_MS);
    connect(&reconnectDelay, &QTimer::timeout, this, &StreamConnection::onReconnectDelayOver);
}

StreamConnection::~StreamConnection()
{
    close();
}

void StreamConnection::apply(const StreamRecord &record)
{
    if (failedNow)
        return;

    switch (record.state) {
    case StreamRecord::State::AwaitingEndpoint:
        // The server is moving the stream; the next update says where to.
        if (!current || endpoint.isEmpty())
            break;
        [[fallthrough]];
    case StreamRecord::State::Reconnecting:
        dropConnection({});
        endpoint.clear();
        if (!connectTimeout.isActive())
            connectTimeout.start();
        emit reconnecting();
        break;
    case StreamRecord::State::Ready:
        if (current && record.endpoint == endpoint && record.token == token)
            break;
        rtcServerId = record.rtcServerId;
        rtcChannelId = record.rtcChannelId;
        endpoint = record.endpoint;
        token = record.token;
        connectNow();
        break;
    }
}

void StreamConnection::close(const std::function<void()> &beforeStop)
{
    connectTimeout.stop();
    reconnectDelay.stop();
    dropConnection(beforeStop);
}

void StreamConnection::connectNow()
{
    dropConnection({});

    Discord::Voice::ConnectionParams params;
    params.endpoint = endpoint;
    params.token = token;
    params.serverId = rtcServerId;
    params.channelId = streamRtcChannelId(rtcServerId, rtcChannelId);
    params.userId = account.userId;
    params.sessionId = account.gatewaySessionId;
    params.proxy = account.proxy;
    params.video = true;
    Discord::Voice::StreamDescription screen;
    screen.type = QStringLiteral("screen");
    screen.rid = QStringLiteral("100");
    screen.quality = 100;
    params.streams = { screen };
    params.streamContext = true;
    params.sendsAudio = false;
    configure(params);

    current = new Audio::VoiceConnection(params, this);
    connect(current, &Audio::VoiceConnection::stateChanged, this, &StreamConnection::onStateChanged);
    connect(current, &Audio::VoiceConnection::disconnected, this, &StreamConnection::onDisconnected);
    emit opened(current);

    if (!current->start()) {
        fail(Failure::ProxyRefused);
        return;
    }
    connectTimeout.start();
}

// Reached on the first connection and again whenever the client has resumed a dropped one.
void StreamConnection::onStateChanged(VoiceClient::State state)
{
    if (state != VoiceClient::State::Connected)
        return;
    connectTimeout.stop();
    reconnects = 0;
    connectedNow = true;
    emit connected();
}

void StreamConnection::onDisconnected()
{
    qCWarning(LogVoice) << "A stream's RTC connection dropped";
    dropConnection({});
    if (++reconnects > MAX_RECONNECTS) {
        fail(Failure::Lost);
        return;
    }
    if (!connectTimeout.isActive())
        connectTimeout.start();
    emit reconnecting();
    emit pingRequested();
    reconnectDelay.start();
}

void StreamConnection::onReconnectDelayOver()
{
    if (!current && !endpoint.isEmpty())
        connectNow();
}

void StreamConnection::onConnectTimedOut()
{
    qCWarning(LogVoice) << "A stream's RTC connection was not up in time";
    fail(Failure::TimedOut);
}

void StreamConnection::dropConnection(const std::function<void()> &beforeStop)
{
    reconnectDelay.stop();
    if (!current)
        return;

    Audio::VoiceConnection *going = std::exchange(current, nullptr);
    connectedNow = false;
    going->stop(beforeStop);
    emit closed(going);
    // Not deleted here: this may be one of its own signals.
    going->deleteLater();
}

void StreamConnection::fail(Failure failure)
{
    if (std::exchange(failedNow, true))
        return;
    close();
    emit failed(failure);
}

} // namespace Core
} // namespace Acheron
