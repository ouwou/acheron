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

void UdpTransport::startIpDiscovery(const QString &ip, int port, quint32 ssrc)
{
    serverAddress = QHostAddress(ip);
    serverPort = static_cast<quint16>(port);
    discoveryPending = true;
    this->ssrc = ssrc;

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
        if (!discoveryPending)
            return;
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
    QByteArray packet(IP_DISCOVERY_PACKET_SIZE, '\0');
    quint16 type = qToBigEndian(IP_DISCOVERY_REQUEST_TYPE);
    quint16 length = qToBigEndian(IP_DISCOVERY_LENGTH);
    quint32 ssrcBE = qToBigEndian(ssrc);

    memcpy(packet.data() + 0, &type, 2);
    memcpy(packet.data() + 2, &length, 2);
    memcpy(packet.data() + 4, &ssrcBE, 4);

    send(packet);
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

void UdpTransport::send(const QByteArray &data)
{
    if (!socket)
        return;

    if (!association) {
        socket->writeDatagram(data, serverAddress, serverPort);
        return;
    }

    if (!association->isEstablished())
        return;

    const QByteArray wrapped = Socks5UdpAssociation::encapsulate(data, serverAddress, serverPort);
    socket->writeDatagram(wrapped, association->relayAddress(), association->relayPort());
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
    const QHostAddress expectedSender = association ? association->relayAddress() : serverAddress;
    const quint16 expectedPort = association ? association->relayPort() : serverPort;

    while (socket && socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(socket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort = 0;
        socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        if (!sender.isEqual(expectedSender, QHostAddress::TolerantConversion) || senderPort != expectedPort) {
            qCDebug(LogVoice) << "Dropping UDP datagram from unexpected sender" << sender << senderPort;
            continue;
        }

        if (association) {
            datagram = Socks5UdpAssociation::decapsulate(datagram);
            if (datagram.isEmpty())
                continue;
        }

        if (discoveryPending && datagram.size() >= IP_DISCOVERY_PACKET_SIZE) {
            parseIpDiscoveryResponse(datagram);
        } else {
            emit datagramReceived(datagram);
        }
    }
}

void UdpTransport::parseIpDiscoveryResponse(const QByteArray &data)
{
    quint16 type;
    memcpy(&type, data.constData(), 2);
    type = qFromBigEndian(type);

    if (type != IP_DISCOVERY_RESPONSE_TYPE) {
        qCDebug(LogVoice) << "Received non-discovery UDP packet during discovery, type:" << type;
        return;
    }

    discoveryPending = false;
    if (discoveryTimer)
        discoveryTimer->stop();

    // null terminated
    const char *ipStart = data.constData() + 8;
    QString discoveredIp = QString::fromUtf8(ipStart);

    quint16 discoveredPort;
    memcpy(&discoveredPort, data.constData() + 72, 2);
    discoveredPort = qFromBigEndian(discoveredPort);

    qCInfo(LogVoice) << "IP Discovery result:" << discoveredIp << ":" << discoveredPort;

    emit ipDiscovered(discoveredIp, static_cast<int>(discoveredPort));
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
