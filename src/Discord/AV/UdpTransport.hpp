#pragma once

#include <QObject>
#include <QTimer>
#include <QUdpSocket>
#include <QHostAddress>

namespace Acheron {
namespace Discord {
namespace AV {

class UdpTransport : public QObject
{
    Q_OBJECT
public:
    explicit UdpTransport(QObject *parent = nullptr);
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

private:
    QUdpSocket *socket = nullptr;
    QTimer *discoveryTimer = nullptr;
    QHostAddress serverAddress;
    quint16 serverPort = 0;
    quint32 ssrc = 0;
    bool discoveryPending = false;

    static constexpr int DISCOVERY_TIMEOUT_MS = 5000;
};

} // namespace AV
} // namespace Discord
} // namespace Acheron
