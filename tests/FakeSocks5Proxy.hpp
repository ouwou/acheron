#pragma once

#include "Core/ProxyConfig.hpp"

#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtEndian>

#include <memory>
#include <utility>

class FakeSocks5Proxy : public QObject
{
public:
    FakeSocks5Proxy()
    {
        control.listen(QHostAddress::LocalHost);
        relay.bind(QHostAddress(QHostAddress::LocalHost));
        connect(&control, &QTcpServer::newConnection, this, &FakeSocks5Proxy::onConnection);
        connect(&relay, &QUdpSocket::readyRead, this, &FakeSocks5Proxy::onRelayData);
    }

    [[nodiscard]] Acheron::Core::ProxyConfig config() const
    {
        Acheron::Core::ProxyConfig proxy;
        proxy.type = Acheron::Core::ProxyConfig::Type::Socks5;
        proxy.host = QStringLiteral("127.0.0.1");
        proxy.port = control.serverPort();
        return proxy;
    }

    [[nodiscard]] quint16 relayPort() const { return relay.localPort(); }

    void sendRawToClient(const QByteArray &datagram) { relay.writeDatagram(datagram, QHostAddress(QHostAddress::LocalHost), clientPort); }

    void closeControlConnections()
    {
        for (const QPointer<QTcpSocket> &client : std::as_const(clients)) {
            if (client)
                client->disconnectFromHost();
        }
    }

    QList<quint16> connectedTo;
    QList<quint16> onwardPorts;
    QList<quint16> relayedTo;
    int datagramsToClient = 0;
    bool associated = false;

private:
    enum class Stage {
        Greeting,
        Request,
        Tunnelling,
        Associated,
    };

    struct Control
    {
        QTcpSocket *client = nullptr;
        QTcpSocket *onward = nullptr;
        QByteArray buffer;
        Stage stage = Stage::Greeting;
    };

    void onConnection()
    {
        while (QTcpSocket *client = control.nextPendingConnection()) {
            clients.append(client);
            auto state = std::make_shared<Control>();
            state->client = client;
            connect(client, &QTcpSocket::readyRead, this, [this, state] { onControlData(state); });
        }
    }

    void onControlData(const std::shared_ptr<Control> &shared)
    {
        Control &state = *shared;
        state.buffer.append(state.client->readAll());

        if (state.stage == Stage::Greeting) {
            if (state.buffer.size() < 2 || state.buffer.size() < 2 + static_cast<uint8_t>(state.buffer.at(1)))
                return;
            state.buffer.remove(0, 2 + static_cast<uint8_t>(state.buffer.at(1)));
            state.client->write(QByteArray("\x05\x00", 2));
            state.stage = Stage::Request;
        }

        if (state.stage == Stage::Request) {
            if (state.buffer.size() < 5)
                return;
            const char command = state.buffer.at(1);
            const char addressType = state.buffer.at(3);
            int addressSize = 4;
            int addressOffset = 4;
            if (addressType == 0x03) {
                addressSize = static_cast<uint8_t>(state.buffer.at(4));
                addressOffset = 5;
            } else if (addressType == 0x04) {
                addressSize = 16;
            }
            const int requestSize = addressOffset + addressSize + 2;
            if (state.buffer.size() < requestSize)
                return;
            const QByteArray address = state.buffer.mid(addressOffset, addressSize);
            const quint16 port = qFromBigEndian<quint16>(state.buffer.constData() + addressOffset + addressSize);
            state.buffer.remove(0, requestSize);

            QByteArray reply("\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00", 10);
            if (command == 0x03) {
                qToBigEndian(relay.localPort(), reply.data() + 8);
                state.client->write(reply);
                state.stage = Stage::Associated;
                associated = true;
                return;
            }

            const QString host = addressType == 0x01 ? QHostAddress(qFromBigEndian<quint32>(address.constData())).toString() : QString::fromLatin1(address);
            state.onward = new QTcpSocket(this);
            QTcpSocket *client = state.client;
            QTcpSocket *onward = state.onward;
            connect(onward, &QTcpSocket::connected, this, [this, shared, port, reply] {
                connectedTo.append(port);
                onwardPorts.append(shared->onward->localPort());
                shared->client->write(reply);
                shared->stage = Stage::Tunnelling;
                shared->onward->write(shared->buffer);
                shared->buffer.clear();
            });
            connect(onward, &QTcpSocket::readyRead, client, [client, onward] { client->write(onward->readAll()); });
            connect(onward, &QTcpSocket::disconnected, client, &QTcpSocket::disconnectFromHost);
            connect(client, &QTcpSocket::disconnected, onward, &QTcpSocket::disconnectFromHost);
            onward->connectToHost(host, port);
            return;
        }

        if (state.stage == Stage::Tunnelling) {
            state.onward->write(state.buffer);
            state.buffer.clear();
        }
    }

    void onRelayData()
    {
        constexpr int RELAY_HEADER_SIZE = 10;
        while (relay.hasPendingDatagrams()) {
            QByteArray datagram(static_cast<int>(relay.pendingDatagramSize()), Qt::Uninitialized);
            quint16 senderPort = 0;
            relay.readDatagram(datagram.data(), datagram.size(), nullptr, &senderPort);

            if (destinations.contains(senderPort)) {
                QByteArray header("\x00\x00\x00\x01\x7f\x00\x00\x01\x00\x00", RELAY_HEADER_SIZE);
                qToBigEndian(senderPort, header.data() + 8);
                relay.writeDatagram(header + datagram, QHostAddress(QHostAddress::LocalHost), clientPort);
                datagramsToClient++;
                continue;
            }

            clientPort = senderPort;
            if (datagram.size() < RELAY_HEADER_SIZE || datagram.at(3) != 0x01)
                continue;
            const quint16 destination = qFromBigEndian<quint16>(datagram.constData() + 8);
            destinations.insert(destination);
            relayedTo.append(destination);
            relay.writeDatagram(datagram.mid(RELAY_HEADER_SIZE), QHostAddress(QHostAddress::LocalHost), destination);
        }
    }

    QTcpServer control;
    QUdpSocket relay;
    QList<QPointer<QTcpSocket>> clients;
    quint16 clientPort = 0;
    QSet<quint16> destinations;
};
