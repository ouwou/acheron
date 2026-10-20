// Tests for the UDP leg of an RTC connection against loopback stand-ins for Discord's media
// server and for a SOCKS5 proxy. The proxied cases assert the property that matters for a
// proxied account: the media server never sees a datagram that did not come from the relay.

#include "Core/ProxyConfig.hpp"
#include "Discord/Voice/PacketBuffer.hpp"
#include "Discord/Voice/UdpTransport.hpp"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUdpSocket>
#include <QtEndian>

using Acheron::Core::ProxyConfig;
using Acheron::Discord::Voice::PacketBuffer;
using Acheron::Discord::Voice::PacketPool;
using Acheron::Discord::Voice::UdpTransport;

namespace {

const QHostAddress LOOPBACK(QHostAddress::LocalHost);
constexpr int IP_DISCOVERY_SIZE = 74;
constexpr int IPV4_RELAY_HEADER_SIZE = 10;

struct Datagram
{
    QByteArray payload;
    quint16 senderPort = 0;
};

class FakeMediaServer : public QObject
{
public:
    FakeMediaServer()
    {
        socket.bind(LOOPBACK, 0);
        connect(&socket, &QUdpSocket::readyRead, this, [this] {
            while (socket.hasPendingDatagrams()) {
                Datagram datagram;
                datagram.payload.resize(int(socket.pendingDatagramSize()));
                socket.readDatagram(datagram.payload.data(), datagram.payload.size(), nullptr, &datagram.senderPort);
                received.append(datagram);
            }
        });
    }

    [[nodiscard]] quint16 port() const { return socket.localPort(); }

    void sendTo(quint16 destinationPort, const QByteArray &payload) { socket.writeDatagram(payload, LOOPBACK, destinationPort); }

    void answerDiscovery(quint16 destinationPort, const QByteArray &externalIp, quint16 externalPort)
    {
        QByteArray response(IP_DISCOVERY_SIZE, '\0');
        qToBigEndian(quint16(2), response.data());
        qToBigEndian(quint16(70), response.data() + 2);
        memcpy(response.data() + 8, externalIp.constData(), size_t(externalIp.size()));
        qToBigEndian(externalPort, response.data() + 72);
        sendTo(destinationPort, response);
    }

    QList<Datagram> received;

private:
    QUdpSocket socket;
};

// Accepts one UDP ASSOCIATE without authentication and relays IPv4 datagrams both ways.
class FakeSocks5Proxy : public QObject
{
public:
    explicit FakeSocks5Proxy(quint16 mediaServerPort) : mediaServerPort(mediaServerPort)
    {
        control.listen(LOOPBACK, 0);
        relay.bind(LOOPBACK, 0);

        connect(&control, &QTcpServer::newConnection, this, [this] {
            client = control.nextPendingConnection();
            connect(client, &QTcpSocket::readyRead, this, [this] { onControlData(); });
        });
        connect(&relay, &QUdpSocket::readyRead, this, [this] { onRelayData(); });
    }

    [[nodiscard]] ProxyConfig config() const
    {
        ProxyConfig proxy;
        proxy.type = ProxyConfig::Type::Socks5;
        proxy.host = LOOPBACK.toString();
        proxy.port = control.serverPort();
        return proxy;
    }

    [[nodiscard]] quint16 relayPort() const { return relay.localPort(); }

    void sendRawToClient(const QByteArray &datagram) { relay.writeDatagram(datagram, LOOPBACK, clientPort); }

    void closeControlConnection()
    {
        if (client)
            client->disconnectFromHost();
    }

    QList<quint16> forwardedDestinationPorts;
    bool associated = false;
    quint16 clientPort = 0;

private:
    void onControlData()
    {
        const QByteArray data = client->readAll();
        if (!greeted) {
            greeted = true;
            client->write(QByteArray("\x05\x00", 2));
            return;
        }
        if (data.size() >= 4 && data.at(1) == 0x03) {
            QByteArray reply("\x05\x00\x00\x01\x7f\x00\x00\x01", 8);
            reply.append(2, '\0');
            qToBigEndian(relay.localPort(), reply.data() + 8);
            client->write(reply);
            associated = true;
        }
    }

    void onRelayData()
    {
        while (relay.hasPendingDatagrams()) {
            QByteArray datagram(int(relay.pendingDatagramSize()), '\0');
            quint16 senderPort = 0;
            relay.readDatagram(datagram.data(), datagram.size(), nullptr, &senderPort);

            if (senderPort == mediaServerPort) {
                QByteArray wrapped("\x00\x00\x00\x01\x7f\x00\x00\x01", 8);
                wrapped.append(2, '\0');
                qToBigEndian(mediaServerPort, wrapped.data() + 8);
                relay.writeDatagram(wrapped + datagram, LOOPBACK, clientPort);
                continue;
            }

            clientPort = senderPort;
            if (datagram.size() < IPV4_RELAY_HEADER_SIZE || datagram.at(3) != 0x01)
                continue;
            const quint16 destinationPort = qFromBigEndian<quint16>(datagram.constData() + 8);
            forwardedDestinationPorts.append(destinationPort);
            relay.writeDatagram(datagram.mid(IPV4_RELAY_HEADER_SIZE), LOOPBACK, destinationPort);
        }
    }

    QTcpServer control;
    QTcpSocket *client = nullptr;
    QUdpSocket relay;
    quint16 mediaServerPort;
    bool greeted = false;
};

QByteArray contentsOf(const PacketBuffer &packet)
{
    return QByteArray(reinterpret_cast<const char *>(packet.data()), packet.size());
}

std::span<const uint8_t> bytesOf(const QByteArray &bytes)
{
    return { reinterpret_cast<const uint8_t *>(bytes.constData()), static_cast<size_t>(bytes.size()) };
}

QByteArray rtpLikePayload(char fill, int size = 1200)
{
    return QByteArray("\x80\x65", 2) + QByteArray(size - 2, fill);
}

} // namespace

class TestUdpTransport : public QObject
{
    Q_OBJECT
private slots:
    void directDiscoveryThenPacketsBothWays();
    void directDropsDatagramsFromOtherSenders();
    void socks5CarriesDiscoveryAndPacketsThroughRelay();
    void socks5DropsFragmentsAndUnwrappedDatagrams();
    void socks5SendsNothingBeforeAssociationOrAfterItFails();
    void socks5ReportsRelayLossAfterDiscovery();
    void anHttpProxyCarriesNothing();
};

void TestUdpTransport::directDiscoveryThenPacketsBothWays()
{
    FakeMediaServer server;
    UdpTransport transport{ ProxyConfig() };
    QList<QByteArray> delivered;
    transport.setPacketHandler([&](PacketPool::Handle packet) { delivered.append(contentsOf(*packet)); });
    QSignalSpy discovered(&transport, &UdpTransport::ipDiscovered);

    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 0x01020304);

    QTRY_COMPARE(server.received.size(), 1);
    const QByteArray request = server.received.first().payload;
    QCOMPARE(request.size(), IP_DISCOVERY_SIZE);
    QCOMPARE(request.left(8), QByteArray::fromHex("0001" "0046" "01020304"));
    QCOMPARE(request.mid(8), QByteArray(IP_DISCOVERY_SIZE - 8, '\0'));
    QCOMPARE(server.received.first().senderPort, transport.localPort());

    server.answerDiscovery(transport.localPort(), "198.51.100.9", 43210);
    QTRY_COMPARE(discovered.size(), 1);
    QCOMPARE(discovered.first().at(0).toString(), QStringLiteral("198.51.100.9"));
    QCOMPARE(discovered.first().at(1).toInt(), 43210);
    QVERIFY(delivered.isEmpty());

    const QByteArray inbound = rtpLikePayload('i');
    server.sendTo(transport.localPort(), inbound);
    QTRY_COMPARE(delivered.size(), 1);
    QCOMPARE(delivered.first(), inbound);

    const QByteArray outbound = rtpLikePayload('o');
    PacketPool::Handle packet = transport.acquirePacket();
    QVERIFY(packet->append(bytesOf(outbound)));
    transport.send(*packet);
    QCOMPARE(contentsOf(*packet), outbound);

    QTRY_COMPARE(server.received.size(), 2);
    QCOMPARE(server.received.last().payload, outbound);
}

void TestUdpTransport::directDropsDatagramsFromOtherSenders()
{
    FakeMediaServer server;
    FakeMediaServer stranger;
    UdpTransport transport{ ProxyConfig() };
    QList<QByteArray> delivered;
    transport.setPacketHandler([&](PacketPool::Handle packet) { delivered.append(contentsOf(*packet)); });
    QSignalSpy discovered(&transport, &UdpTransport::ipDiscovered);

    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 1);
    QTRY_COMPARE(server.received.size(), 1);

    stranger.answerDiscovery(transport.localPort(), "203.0.113.66", 1);
    stranger.sendTo(transport.localPort(), rtpLikePayload('x'));
    server.answerDiscovery(transport.localPort(), "198.51.100.9", 43210);
    server.sendTo(transport.localPort(), rtpLikePayload('s'));

    QTRY_COMPARE(delivered.size(), 1);
    QCOMPARE(delivered.first(), rtpLikePayload('s'));
    QCOMPARE(discovered.size(), 1);
    QCOMPARE(discovered.first().at(0).toString(), QStringLiteral("198.51.100.9"));
}

void TestUdpTransport::socks5CarriesDiscoveryAndPacketsThroughRelay()
{
    FakeMediaServer server;
    FakeSocks5Proxy proxy(server.port());
    UdpTransport transport{ proxy.config() };
    QList<QByteArray> delivered;
    transport.setPacketHandler([&](PacketPool::Handle packet) { delivered.append(contentsOf(*packet)); });
    QSignalSpy discovered(&transport, &UdpTransport::ipDiscovered);

    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 0x0a0b0c0d);

    QTRY_COMPARE(server.received.size(), 1);
    QVERIFY(proxy.associated);
    QCOMPARE(server.received.first().payload.size(), IP_DISCOVERY_SIZE);
    QCOMPARE(server.received.first().payload.left(8), QByteArray::fromHex("0001" "0046" "0a0b0c0d"));

    server.answerDiscovery(proxy.relayPort(), "198.51.100.9", 43210);
    QTRY_COMPARE(discovered.size(), 1);
    QCOMPARE(discovered.first().at(0).toString(), QStringLiteral("198.51.100.9"));

    const QByteArray inbound = rtpLikePayload('i');
    server.sendTo(proxy.relayPort(), inbound);
    QTRY_COMPARE(delivered.size(), 1);
    QCOMPARE(delivered.first(), inbound);

    const QByteArray outbound = rtpLikePayload('o');
    PacketPool::Handle packet = transport.acquirePacket();
    QVERIFY(packet->append(bytesOf(outbound)));
    const uint8_t *contentAddress = packet->data();
    transport.send(*packet);

    // The relay header lived in the packet's headroom only for the duration of the send.
    QCOMPARE(contentsOf(*packet), outbound);
    QCOMPARE(static_cast<const void *>(packet->data()), static_cast<const void *>(contentAddress));

    QTRY_COMPARE(server.received.size(), 2);
    QCOMPARE(server.received.last().payload, outbound);

    // Nothing reached the media server except through the relay, addressed to the media server.
    for (const Datagram &datagram : server.received) {
        QCOMPARE(datagram.senderPort, proxy.relayPort());
        QVERIFY(datagram.senderPort != transport.localPort());
    }
    QCOMPARE(proxy.forwardedDestinationPorts, (QList<quint16>{ server.port(), server.port() }));
}

void TestUdpTransport::socks5DropsFragmentsAndUnwrappedDatagrams()
{
    FakeMediaServer server;
    FakeSocks5Proxy proxy(server.port());
    UdpTransport transport{ proxy.config() };
    QList<QByteArray> delivered;
    transport.setPacketHandler([&](PacketPool::Handle packet) { delivered.append(contentsOf(*packet)); });
    QSignalSpy discovered(&transport, &UdpTransport::ipDiscovered);

    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 1);
    QTRY_COMPARE(server.received.size(), 1);
    server.answerDiscovery(proxy.relayPort(), "198.51.100.9", 43210);
    QTRY_COMPARE(discovered.size(), 1);

    const QByteArray payload = rtpLikePayload('p', 200);
    const QByteArray header("\x00\x00\x00\x01\x7f\x00\x00\x01\x4b\x86", 10);

    QByteArray fragment = header;
    fragment[2] = char(1);
    proxy.sendRawToClient(fragment + payload);
    proxy.sendRawToClient(QByteArray("\x00\x00\x00\x09", 4) + payload);
    proxy.sendRawToClient(QByteArray("\x00\x00", 2));

    // A datagram straight from the media server, bypassing the relay, must not be trusted.
    server.sendTo(transport.localPort(), payload);

    const QByteArray marker = rtpLikePayload('m', 200);
    proxy.sendRawToClient(header + marker);

    QTRY_COMPARE(delivered.size(), 1);
    QCOMPARE(delivered.first(), marker);
}

void TestUdpTransport::socks5SendsNothingBeforeAssociationOrAfterItFails()
{
    FakeMediaServer server;

    // A proxy that accepts the TCP connection and then never answers.
    QTcpServer silentProxy;
    QVERIFY(silentProxy.listen(LOOPBACK, 0));
    ProxyConfig config;
    config.type = ProxyConfig::Type::Socks5;
    config.host = LOOPBACK.toString();
    config.port = silentProxy.serverPort();

    UdpTransport transport{ config };
    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 1);

    PacketPool::Handle packet = transport.acquirePacket();
    QVERIFY(packet->append(bytesOf(rtpLikePayload('l'))));
    transport.send(*packet);

    QTest::qWait(200);
    QVERIFY(server.received.isEmpty());

    // And once an association is lost, the transport reports failure instead of going direct.
    FakeSocks5Proxy proxy(server.port());
    UdpTransport proxied{ proxy.config() };
    QSignalSpy failed(&proxied, &UdpTransport::ipDiscoveryFailed);
    proxied.startIpDiscovery(LOOPBACK.toString(), server.port(), 1);
    QTRY_COMPARE(server.received.size(), 1);

    proxy.closeControlConnection();
    QTRY_COMPARE(failed.size(), 1);

    const int before = server.received.size();
    proxied.send(*packet);
    QTest::qWait(200);
    QCOMPARE(server.received.size(), before);
}

void TestUdpTransport::socks5ReportsRelayLossAfterDiscovery()
{
    FakeMediaServer server;
    FakeSocks5Proxy proxy(server.port());
    UdpTransport transport{ proxy.config() };
    QSignalSpy discovered(&transport, &UdpTransport::ipDiscovered);
    QSignalSpy discoveryFailed(&transport, &UdpTransport::ipDiscoveryFailed);
    QSignalSpy relayLost(&transport, &UdpTransport::relayLost);

    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 1);
    QTRY_COMPARE(server.received.size(), 1);
    server.answerDiscovery(proxy.relayPort(), "198.51.100.9", 43210);
    QTRY_COMPARE(discovered.size(), 1);

    // RFC 1928 ties the relay's life to the control connection, so its loss ends the media path.
    proxy.closeControlConnection();
    QTRY_COMPARE(relayLost.size(), 1);
    QVERIFY(discoveryFailed.isEmpty());

    PacketPool::Handle packet = transport.acquirePacket();
    QVERIFY(packet->append(bytesOf(rtpLikePayload('l'))));
    transport.send(*packet);
    QTest::qWait(200);
    QCOMPARE(server.received.size(), 1);
}

void TestUdpTransport::anHttpProxyCarriesNothing()
{
    FakeMediaServer server;
    ProxyConfig config;
    config.type = ProxyConfig::Type::Http;
    config.host = LOOPBACK.toString();
    config.port = 8080;

    UdpTransport transport{ config };
    QSignalSpy failed(&transport, &UdpTransport::ipDiscoveryFailed);
    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 1);

    PacketPool::Handle packet = transport.acquirePacket();
    QVERIFY(packet->append(bytesOf(rtpLikePayload('h'))));
    transport.send(*packet);

    QTRY_COMPARE(failed.size(), 1);
    QTest::qWait(200);
    QVERIFY(server.received.isEmpty());
    QVERIFY(!transport.isBound());
}

QTEST_GUILESS_MAIN(TestUdpTransport)
#include "tst_UdpTransport.moc"
