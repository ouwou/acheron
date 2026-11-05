#include "Core/ProxyConfig.hpp"
#include "Discord/Voice/PacketBuffer.hpp"
#include "Discord/Voice/UdpTransport.hpp"
#include "FakeSocks5Proxy.hpp"
#include "PacketBytes.hpp"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTest>
#include <QUdpSocket>
#include <QtEndian>

using Acheron::Core::ProxyConfig;
using Acheron::Discord::Voice::PacketPool;
using Acheron::Discord::Voice::UdpTransport;
using PacketBytes::bytesOf;
using PacketBytes::contentsOf;

namespace {

const QHostAddress LOOPBACK(QHostAddress::LocalHost);
constexpr int IP_DISCOVERY_SIZE = 74;

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

class RecordingTransport : public UdpTransport
{
public:
    explicit RecordingTransport(const ProxyConfig &proxy) : UdpTransport(proxy)
    {
        setPacketHandler([this](PacketPool::Handle packet) { delivered.append(contentsOf(*packet)); });
    }

    QList<QByteArray> delivered;
    QSignalSpy discovered{ this, &UdpTransport::ipDiscovered };
};

QByteArray rtpLikePayload(char fill, int size = 1200)
{
    return QByteArray("\x80\x65", 2) + QByteArray(size - 2, fill);
}

[[nodiscard]] bool sendPacket(UdpTransport &transport, const QByteArray &payload)
{
    PacketPool::Handle packet = transport.acquirePacket();
    if (!packet->append(bytesOf(payload)))
        return false;
    transport.send(*packet);
    return true;
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
    RecordingTransport transport{ ProxyConfig() };

    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 0x01020304);

    QTRY_COMPARE(server.received.size(), 1);
    const QByteArray request = server.received.first().payload;
    QCOMPARE(request.size(), IP_DISCOVERY_SIZE);
    QCOMPARE(request.left(8), QByteArray::fromHex("0001"
                                                  "0046"
                                                  "01020304"));
    QCOMPARE(request.mid(8), QByteArray(IP_DISCOVERY_SIZE - 8, '\0'));
    QCOMPARE(server.received.first().senderPort, transport.localPort());

    server.answerDiscovery(transport.localPort(), "198.51.100.9", 43210);
    QTRY_COMPARE(transport.discovered.size(), 1);
    QCOMPARE(transport.discovered.first().at(0).toString(), QStringLiteral("198.51.100.9"));
    QCOMPARE(transport.discovered.first().at(1).toInt(), 43210);
    QVERIFY(transport.delivered.isEmpty());

    const QByteArray inbound = rtpLikePayload('i');
    server.sendTo(transport.localPort(), inbound);
    QTRY_COMPARE(transport.delivered.size(), 1);
    QCOMPARE(transport.delivered.first(), inbound);

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
    RecordingTransport transport{ ProxyConfig() };

    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 1);
    QTRY_COMPARE(server.received.size(), 1);

    stranger.answerDiscovery(transport.localPort(), "203.0.113.66", 1);
    stranger.sendTo(transport.localPort(), rtpLikePayload('x'));
    server.answerDiscovery(transport.localPort(), "198.51.100.9", 43210);
    server.sendTo(transport.localPort(), rtpLikePayload('s'));

    QTRY_COMPARE(transport.delivered.size(), 1);
    QCOMPARE(transport.delivered.first(), rtpLikePayload('s'));
    QCOMPARE(transport.discovered.size(), 1);
    QCOMPARE(transport.discovered.first().at(0).toString(), QStringLiteral("198.51.100.9"));
}

void TestUdpTransport::socks5CarriesDiscoveryAndPacketsThroughRelay()
{
    FakeMediaServer server;
    FakeSocks5Proxy proxy;
    RecordingTransport transport{ proxy.config() };

    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 0x0a0b0c0d);

    QTRY_COMPARE(server.received.size(), 1);
    QVERIFY(proxy.associated);
    QCOMPARE(server.received.first().payload.size(), IP_DISCOVERY_SIZE);
    QCOMPARE(server.received.first().payload.left(8), QByteArray::fromHex("0001"
                                                                          "0046"
                                                                          "0a0b0c0d"));

    server.answerDiscovery(proxy.relayPort(), "198.51.100.9", 43210);
    QTRY_COMPARE(transport.discovered.size(), 1);
    QCOMPARE(transport.discovered.first().at(0).toString(), QStringLiteral("198.51.100.9"));

    const QByteArray inbound = rtpLikePayload('i');
    server.sendTo(proxy.relayPort(), inbound);
    QTRY_COMPARE(transport.delivered.size(), 1);
    QCOMPARE(transport.delivered.first(), inbound);

    const QByteArray outbound = rtpLikePayload('o');
    PacketPool::Handle packet = transport.acquirePacket();
    QVERIFY(packet->append(bytesOf(outbound)));
    const uint8_t *contentAddress = packet->data();
    transport.send(*packet);

    QCOMPARE(contentsOf(*packet), outbound);
    QCOMPARE(static_cast<const void *>(packet->data()), static_cast<const void *>(contentAddress));

    QTRY_COMPARE(server.received.size(), 2);
    QCOMPARE(server.received.last().payload, outbound);

    for (const Datagram &datagram : server.received) {
        QCOMPARE(datagram.senderPort, proxy.relayPort());
        QVERIFY(datagram.senderPort != transport.localPort());
    }
    QCOMPARE(proxy.relayedTo, (QList<quint16>{ server.port(), server.port() }));
}

void TestUdpTransport::socks5DropsFragmentsAndUnwrappedDatagrams()
{
    FakeMediaServer server;
    FakeSocks5Proxy proxy;
    RecordingTransport transport{ proxy.config() };

    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 1);
    QTRY_COMPARE(server.received.size(), 1);
    server.answerDiscovery(proxy.relayPort(), "198.51.100.9", 43210);
    QTRY_COMPARE(transport.discovered.size(), 1);

    const QByteArray payload = rtpLikePayload('p', 200);
    const QByteArray header("\x00\x00\x00\x01\x7f\x00\x00\x01\x4b\x86", 10);

    QByteArray fragment = header;
    fragment[2] = char(1);
    proxy.sendRawToClient(fragment + payload);
    proxy.sendRawToClient(QByteArray("\x00\x00\x00\x09", 4) + payload);
    proxy.sendRawToClient(QByteArray("\x00\x00", 2));

    server.sendTo(transport.localPort(), payload);

    const QByteArray marker = rtpLikePayload('m', 200);
    proxy.sendRawToClient(header + marker);

    QTRY_COMPARE(transport.delivered.size(), 1);
    QCOMPARE(transport.delivered.first(), marker);
}

void TestUdpTransport::socks5SendsNothingBeforeAssociationOrAfterItFails()
{
    FakeMediaServer server;

    QTcpServer silentProxy;
    QVERIFY(silentProxy.listen(LOOPBACK, 0));
    ProxyConfig config;
    config.type = ProxyConfig::Type::Socks5;
    config.host = LOOPBACK.toString();
    config.port = silentProxy.serverPort();

    UdpTransport transport{ config };
    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 1);
    QVERIFY(sendPacket(transport, rtpLikePayload('l')));

    QTest::qWait(200);
    QVERIFY(server.received.isEmpty());

    FakeSocks5Proxy proxy;
    UdpTransport proxied{ proxy.config() };
    QSignalSpy failed(&proxied, &UdpTransport::ipDiscoveryFailed);
    proxied.startIpDiscovery(LOOPBACK.toString(), server.port(), 1);
    QTRY_COMPARE(server.received.size(), 1);

    proxy.closeControlConnections();
    QTRY_COMPARE(failed.size(), 1);

    const int before = server.received.size();
    QVERIFY(sendPacket(proxied, rtpLikePayload('l')));
    QTest::qWait(200);
    QCOMPARE(server.received.size(), before);
}

void TestUdpTransport::socks5ReportsRelayLossAfterDiscovery()
{
    FakeMediaServer server;
    FakeSocks5Proxy proxy;
    RecordingTransport transport{ proxy.config() };
    QSignalSpy discoveryFailed(&transport, &UdpTransport::ipDiscoveryFailed);
    QSignalSpy relayLost(&transport, &UdpTransport::relayLost);

    transport.startIpDiscovery(LOOPBACK.toString(), server.port(), 1);
    QTRY_COMPARE(server.received.size(), 1);
    server.answerDiscovery(proxy.relayPort(), "198.51.100.9", 43210);
    QTRY_COMPARE(transport.discovered.size(), 1);

    proxy.closeControlConnections();
    QTRY_COMPARE(relayLost.size(), 1);
    QVERIFY(discoveryFailed.isEmpty());

    QVERIFY(sendPacket(transport, rtpLikePayload('l')));
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
    QVERIFY(sendPacket(transport, rtpLikePayload('h')));

    QTRY_COMPARE(failed.size(), 1);
    QTest::qWait(200);
    QVERIFY(server.received.isEmpty());
    QVERIFY(!transport.isBound());
}

QTEST_GUILESS_MAIN(TestUdpTransport)
#include "tst_UdpTransport.moc"
