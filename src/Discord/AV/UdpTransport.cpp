#include "UdpTransport.hpp"

#include "Core/Logging.hpp"

#include <QDataStream>
#include <QtEndian>

namespace Acheron {
namespace Discord {
namespace AV {

static constexpr int IP_DISCOVERY_PACKET_SIZE = 74;
static constexpr quint16 IP_DISCOVERY_REQUEST_TYPE = 0x0001;
static constexpr quint16 IP_DISCOVERY_RESPONSE_TYPE = 0x0002;
static constexpr quint16 IP_DISCOVERY_LENGTH = 70;

UdpTransport::UdpTransport(QObject *parent)
    : QObject(parent)
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

    if (socket) {
        socket->close();
        delete socket;
    }

    socket = new QUdpSocket(this);
    connect(socket, &QUdpSocket::readyRead, this, &UdpTransport::onReadyRead);

    if (!socket->bind(QHostAddress::AnyIPv4, 0)) {
        qCWarning(LogVoice) << "Failed to bind UDP socket:" << socket->errorString();
        emit ipDiscoveryFailed("Failed to bind UDP socket: " + socket->errorString());
        return;
    }

    qCInfo(LogVoice) << "UDP socket bound to port" << socket->localPort()
                     << "- starting IP discovery to" << ip << ":" << serverPort;

    QByteArray packet(IP_DISCOVERY_PACKET_SIZE, '\0');
    quint16 type = qToBigEndian(IP_DISCOVERY_REQUEST_TYPE);
    quint16 length = qToBigEndian(IP_DISCOVERY_LENGTH);
    quint32 ssrcBE = qToBigEndian(ssrc);

    memcpy(packet.data() + 0, &type, 2);
    memcpy(packet.data() + 2, &length, 2);
    memcpy(packet.data() + 4, &ssrcBE, 4);

    qint64 sent = socket->writeDatagram(packet, serverAddress, this->serverPort);
    if (sent != IP_DISCOVERY_PACKET_SIZE)
        qCWarning(LogVoice) << "IP discovery send failed, wrote" << sent << "bytes";

    // give up after some time
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
    socket->writeDatagram(data, serverAddress, serverPort);
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
    while (socket && socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(socket->pendingDatagramSize());
        socket->readDatagram(datagram.data(), datagram.size());

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

} // namespace AV
} // namespace Discord
} // namespace Acheron
