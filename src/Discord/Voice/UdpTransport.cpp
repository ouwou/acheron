#include "UdpTransport.hpp"

#include "Core/Logging.hpp"

#include <QDataStream>
#include <QtEndian>

namespace Acheron {
namespace Discord {
namespace Voice {

static constexpr int IP_DISCOVERY_PACKET_SIZE = 74;
static constexpr quint16 IP_DISCOVERY_REQUEST_TYPE = 0x0001;
static constexpr quint16 IP_DISCOVERY_RESPONSE_TYPE = 0x0002;
static constexpr quint16 IP_DISCOVERY_LENGTH = 70;
static constexpr int IP_DISCOVERY_ADDRESS_OFFSET = 8;
static constexpr int IP_DISCOVERY_PORT_OFFSET = 72;

UdpTransport::UdpTransport(const Core::ProxyConfig &proxy, QObject *parent)
    : QObject(parent), proxy(proxy)
{
}

UdpTransport::~UdpTransport()
{
    if (socket) {
        socket->close();
        delete socket;
    }
}

void UdpTransport::setPacketHandler(PacketHandler handler)
{
    packetHandler = std::move(handler);
}

PacketPool::Handle UdpTransport::acquirePacket()
{
    return pool.acquire();
}

void UdpTransport::startIpDiscovery(const QString &ip, int port, quint32 ssrc)
{
    serverAddress = QHostAddress(ip);
    serverPort = static_cast<quint16>(port);
    relayHeaderSize = Socks5UdpAssociation::writeRelayHeader(relayHeader, serverAddress, serverPort);
    discoveryPending = true;
    this->ssrc = ssrc;

    // callers refuse such a proxy first; this keeps a missed check from going direct
    if (!proxy.canRelayUdp()) {
        discoveryPending = false;
        QMetaObject::invokeMethod(this, [this] { emit ipDiscoveryFailed(QStringLiteral("an HTTP proxy cannot relay UDP")); }, Qt::QueuedConnection);
        return;
    }

    if (!openSocket())
        return;

    qCInfo(LogVoice) << "UDP socket bound to port" << socket->localPort()
                     << "- starting IP discovery to" << ip << ":" << serverPort;

    startDiscoveryTimeout();

    if (proxy.type != Core::ProxyConfig::Type::Socks5) {
        sendDiscoveryPacket();
        return;
    }

    if (association)
        association->deleteLater();

    association = new Socks5UdpAssociation(this);
    connect(association, &Socks5UdpAssociation::established, this, &UdpTransport::sendDiscoveryPacket);
    connect(association, &Socks5UdpAssociation::failed, this, [this](const QString &error) {
        if (!discoveryPending) {
            emit relayLost(error);
            return;
        }
        discoveryPending = false;
        discoveryTimer->stop();
        emit ipDiscoveryFailed("SOCKS5 proxy: " + error);
    });
    association->open(proxy);
}

bool UdpTransport::openSocket()
{
    if (socket) {
        socket->close();
        delete socket;
    }

    socket = new QUdpSocket(this);
    connect(socket, &QUdpSocket::readyRead, this, &UdpTransport::onReadyRead);

    if (!socket->bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
        qCWarning(LogVoice) << "Failed to bind UDP socket:" << socket->errorString();
        emit ipDiscoveryFailed("Failed to bind UDP socket: " + socket->errorString());
        return false;
    }

    return true;
}

void UdpTransport::sendDiscoveryPacket()
{
    PacketPool::Handle packet = pool.acquire();
    packet->resize(IP_DISCOVERY_PACKET_SIZE);
    memset(packet->data(), 0, IP_DISCOVERY_PACKET_SIZE);

    qToBigEndian(IP_DISCOVERY_REQUEST_TYPE, packet->data() + 0);
    qToBigEndian(IP_DISCOVERY_LENGTH, packet->data() + 2);
    qToBigEndian(ssrc, packet->data() + 4);

    send(*packet);
}

void UdpTransport::startDiscoveryTimeout()
{
    if (!discoveryTimer) {
        discoveryTimer = new QTimer(this);
        discoveryTimer->setSingleShot(true);
        connect(discoveryTimer, &QTimer::timeout, this, [this] {
            if (discoveryPending) {
                discoveryPending = false;
                emit ipDiscoveryFailed("IP discovery timed out");
            }
        });
    }
    discoveryTimer->start(DISCOVERY_TIMEOUT_MS);
}

void UdpTransport::send(PacketBuffer &packet)
{
    if (!socket)
        return;

    if (!relayed()) {
        socket->writeDatagram(reinterpret_cast<const char *>(packet.data()), packet.size(), serverAddress, serverPort);
        return;
    }

    if (!association || !association->isEstablished() || relayHeaderSize <= 0)
        return;

    if (!Socks5UdpAssociation::wrap(packet, { relayHeader, static_cast<size_t>(relayHeaderSize) }))
        return;
    socket->writeDatagram(reinterpret_cast<const char *>(packet.data()), packet.size(), association->relayAddress(), association->relayPort());
    packet.dropFront(relayHeaderSize);
}

bool UdpTransport::isBound() const
{
    return socket && socket->state() == QAbstractSocket::BoundState;
}

quint16 UdpTransport::localPort() const
{
    return socket ? socket->localPort() : 0;
}

void UdpTransport::onReadyRead()
{
    if (relayed() && !association)
        return;
    const QHostAddress expectedSender = relayed() ? association->relayAddress() : serverAddress;
    const quint16 expectedPort = relayed() ? association->relayPort() : serverPort;

    while (socket && socket->hasPendingDatagrams()) {
        PacketPool::Handle packet = pool.acquire();
        QHostAddress sender;
        quint16 senderPort = 0;
        const qint64 received = socket->readDatagram(reinterpret_cast<char *>(packet->data()), PacketBuffer::MAX_CONTENT, &sender, &senderPort);
        if (received < 0 || !packet->resize(static_cast<int>(received)))
            continue;

        if (!sender.isEqual(expectedSender, QHostAddress::TolerantConversion) || senderPort != expectedPort) {
            qCDebug(LogVoice) << "Dropping UDP datagram from unexpected sender" << sender << senderPort;
            continue;
        }

        if (relayed() && !Socks5UdpAssociation::unwrap(*packet))
            continue;

        if (discoveryPending && packet->size() >= IP_DISCOVERY_PACKET_SIZE)
            parseIpDiscoveryResponse(packet->bytes());
        else if (packetHandler)
            packetHandler(std::move(packet));
    }
}

void UdpTransport::parseIpDiscoveryResponse(std::span<const uint8_t> data)
{
    const quint16 type = qFromBigEndian<quint16>(data.data());

    if (type != IP_DISCOVERY_RESPONSE_TYPE) {
        qCDebug(LogVoice) << "Received non-discovery UDP packet during discovery, type:" << type;
        return;
    }

    discoveryPending = false;
    if (discoveryTimer)
        discoveryTimer->stop();

    // null terminated
    const char *ipStart = reinterpret_cast<const char *>(data.data()) + IP_DISCOVERY_ADDRESS_OFFSET;
    QString discoveredIp = QString::fromUtf8(ipStart, static_cast<int>(qstrnlen(ipStart, IP_DISCOVERY_PORT_OFFSET - IP_DISCOVERY_ADDRESS_OFFSET)));

    const quint16 discoveredPort = qFromBigEndian<quint16>(data.data() + IP_DISCOVERY_PORT_OFFSET);

    qCInfo(LogVoice) << "IP Discovery result:" << discoveredIp << ":" << discoveredPort;

    emit ipDiscovered(discoveredIp, static_cast<int>(discoveredPort));
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
