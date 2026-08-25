#pragma once

#include <QHostAddress>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>

#include "Core/ProxyConfig.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

// RFC 1928 UDP ASSOCIATE
class Socks5UdpAssociation : public QObject
{
    Q_OBJECT
public:
    explicit Socks5UdpAssociation(QObject *parent = nullptr);

    void open(const Core::ProxyConfig &proxy);
    void close();

    [[nodiscard]] bool isEstablished() const { return stage == Stage::Established; }
    [[nodiscard]] QHostAddress relayAddress() const { return relay; }
    [[nodiscard]] quint16 relayPort() const { return relayPortNumber; }

    static QByteArray encapsulate(const QByteArray &payload, const QHostAddress &destination, quint16 destinationPort);
    static QByteArray decapsulate(const QByteArray &datagram);

signals:
    void established();
    void failed(const QString &error);

private:
    enum class Stage {
        Idle,
        AwaitingMethod,
        AwaitingAuthReply,
        AwaitingAssociateReply,
        Established,
        Failed,
    };

    void sendGreeting();
    void sendCredentials();
    void sendAssociateRequest();
    void onReadyRead();
    bool consumeMethodReply();
    bool consumeAuthReply();
    bool consumeAssociateReply();
    void fail(const QString &error);
    void releaseControlSocket();

    Core::ProxyConfig proxy;
    QTcpSocket *control = nullptr;
    QTimer timeout;
    QByteArray buffer;
    Stage stage = Stage::Idle;

    QHostAddress relay;
    quint16 relayPortNumber = 0;

    static constexpr int HANDSHAKE_TIMEOUT_MS = 10000;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
