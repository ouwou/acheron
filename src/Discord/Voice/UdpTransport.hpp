#pragma once

#include <QObject>
#include <QTimer>
#include <QUdpSocket>
#include <QHostAddress>

#include <functional>
#include <span>

#include "Core/ProxyConfig.hpp"
#include "PacketBuffer.hpp"
#include "Socks5UdpAssociation.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

class UdpTransport : public QObject
{
    Q_OBJECT
public:
    explicit UdpTransport(const Core::ProxyConfig &proxy, QObject *parent = nullptr);
    ~UdpTransport() override;

    // Called on this object's thread with each datagram that is not part of IP discovery. A
    // handler rather than a signal, because the receiver takes ownership of the packet.
    using PacketHandler = std::function<void(PacketPool::Handle)>;
    void setPacketHandler(PacketHandler handler);

    void startIpDiscovery(const QString &ip, int port, quint32 ssrc);

    [[nodiscard]] PacketPool::Handle acquirePacket();
    // The packet is unchanged on return; through a relay its headroom carries the relay header
    // while the datagram is written.
    void send(PacketBuffer &packet);

    [[nodiscard]] bool isBound() const;
    [[nodiscard]] quint16 localPort() const;

signals:
    void ipDiscovered(const QString &externalIp, int externalPort);
    void ipDiscoveryFailed(const QString &error);
    // The SOCKS5 association ended after discovery. Nothing can be sent any more: the transport
    // never falls back to a direct route.
    void relayLost(const QString &error);

private slots:
    void onReadyRead();

private:
    void parseIpDiscoveryResponse(std::span<const uint8_t> data);
    bool openSocket();
    void sendDiscoveryPacket();
    void startDiscoveryTimeout();

    QUdpSocket *socket = nullptr;
    QTimer *discoveryTimer = nullptr;
    QHostAddress serverAddress;
    quint16 serverPort = 0;
    quint32 ssrc = 0;
    bool discoveryPending = false;

    // Through the SOCKS5 proxy, and never past it, whether or not its association is up.
    [[nodiscard]] bool relayed() const { return proxy.type == Core::ProxyConfig::Type::Socks5; }

    Core::ProxyConfig proxy;
    Socks5UdpAssociation *association = nullptr;
    uint8_t relayHeader[Socks5UdpAssociation::MAX_RELAY_HEADER_SIZE] = {};
    int relayHeaderSize = 0;

    PacketPool pool;
    PacketHandler packetHandler;

    static constexpr int DISCOVERY_TIMEOUT_MS = 5000;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
