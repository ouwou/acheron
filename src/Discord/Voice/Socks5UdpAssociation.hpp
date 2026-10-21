#pragma once

#include <QHostAddress>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>

#include <cstdint>
#include <span>

#include "Core/ProxyConfig.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

class PacketBuffer;

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

    // RSV (two bytes) and FRAG
    static constexpr int DATAGRAM_PREFIX_SIZE = 3;
    static constexpr int IPV4_ADDRESS_FIELD_SIZE = 1 + 4 + 2;
    static constexpr int IPV6_ADDRESS_FIELD_SIZE = 1 + 16 + 2;
    static constexpr int MAX_RELAY_HEADER_SIZE = DATAGRAM_PREFIX_SIZE + IPV6_ADDRESS_FIELD_SIZE;
    static int writeRelayHeader(std::span<uint8_t> out, const QHostAddress &destination, quint16 destinationPort);

    static bool wrap(PacketBuffer &packet, std::span<const uint8_t> relayHeader);
    static bool unwrap(PacketBuffer &packet);

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
