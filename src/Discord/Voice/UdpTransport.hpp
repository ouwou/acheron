#pragma once

#include <QObject>
#include <QTimer>
#include <QUdpSocket>
#include <QHostAddress>

#include "Core/ProxyConfig.hpp"
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

    void startIpDiscovery(const QString &ip, int port, quint32 ssrc);
    void send(const QByteArray &data);

    [[nodiscard]] bool isBound() const;
    [[nodiscard]] quint16 localPort() const;

signals:
    void ipDiscovered(const QString &externalIp, int externalPort);
    void ipDiscoveryFailed(const QString &error);
    void datagramReceived(const QByteArray &data);

private slots:
    void onReadyRead();

private:
    void parseIpDiscoveryResponse(const QByteArray &data);
    bool openSocket();
    void sendDiscoveryPacket();
    void startDiscoveryTimeout();

    QUdpSocket *socket = nullptr;
    QTimer *discoveryTimer = nullptr;
    QHostAddress serverAddress;
    quint16 serverPort = 0;
    quint32 ssrc = 0;
    bool discoveryPending = false;

    Core::ProxyConfig proxy;
    Socks5UdpAssociation *association = nullptr;

    static constexpr int DISCOVERY_TIMEOUT_MS = 5000;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
