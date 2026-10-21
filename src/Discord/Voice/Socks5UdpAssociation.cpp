#include "Socks5UdpAssociation.hpp"

#include "PacketBuffer.hpp"

#include "Core/Logging.hpp"

#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

constexpr char SOCKS_VERSION = 0x05;
constexpr char AUTH_VERSION = 0x01;
constexpr char METHOD_NONE = 0x00;
constexpr char METHOD_USERPASS = 0x02;
constexpr char METHOD_UNACCEPTABLE = char(0xFF);
constexpr char CMD_UDP_ASSOCIATE = 0x03;
constexpr char ATYP_IPV4 = 0x01;
constexpr char ATYP_DOMAIN = 0x03;
constexpr char ATYP_IPV6 = 0x04;
constexpr char REPLY_SUCCEEDED = 0x00;

int addressFieldSize(const QHostAddress &address)
{
    return address.protocol() == QAbstractSocket::IPv6Protocol ? Socks5UdpAssociation::IPV6_ADDRESS_FIELD_SIZE : Socks5UdpAssociation::IPV4_ADDRESS_FIELD_SIZE;
}

// ATYP, ADDR and PORT
void writeAddress(uint8_t *out, const QHostAddress &address, quint16 port)
{
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        *out++ = ATYP_IPV6;
        const Q_IPV6ADDR raw = address.toIPv6Address();
        std::memcpy(out, raw.c, 16);
        out += 16;
    } else {
        *out++ = ATYP_IPV4;
        qToBigEndian(address.toIPv4Address(), out);
        out += 4;
    }
    qToBigEndian(port, out);
}

void appendAddress(QByteArray &out, const QHostAddress &address, quint16 port)
{
    const auto at = out.size();
    out.resize(at + addressFieldSize(address));
    writeAddress(reinterpret_cast<uint8_t *>(out.data()) + at, address, port);
}

// Length of the ATYP+ADDR+PORT tail starting at `offset`, or -1 if incomplete or unknown.
int addressFieldLength(const char *data, int size, int offset)
{
    if (size <= offset)
        return -1;

    switch (data[offset]) {
    case ATYP_IPV4:
        return Socks5UdpAssociation::IPV4_ADDRESS_FIELD_SIZE;
    case ATYP_IPV6:
        return Socks5UdpAssociation::IPV6_ADDRESS_FIELD_SIZE;
    case ATYP_DOMAIN:
        if (size <= offset + 1)
            return -1;
        return 1 + 1 + static_cast<quint8>(data[offset + 1]) + 2;
    default:
        return -1;
    }
}

QString replyErrorString(quint8 code)
{
    switch (code) {
    case 0x01:
        return QStringLiteral("general SOCKS server failure");
    case 0x02:
        return QStringLiteral("connection not allowed by ruleset");
    case 0x03:
        return QStringLiteral("network unreachable");
    case 0x04:
        return QStringLiteral("host unreachable");
    case 0x05:
        return QStringLiteral("connection refused");
    case 0x06:
        return QStringLiteral("TTL expired");
    case 0x07:
        return QStringLiteral("command not supported");
    case 0x08:
        return QStringLiteral("address type not supported");
    default:
        return QStringLiteral("unknown error %1").arg(code);
    }
}

} // namespace

Socks5UdpAssociation::Socks5UdpAssociation(QObject *parent)
    : QObject(parent)
{
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, this,
            [this] { fail(QStringLiteral("SOCKS5 handshake timed out")); });
}

void Socks5UdpAssociation::open(const Core::ProxyConfig &config)
{
    close();

    proxy = config;
    stage = Stage::AwaitingMethod;

    control = new QTcpSocket(this);
    connect(control, &QTcpSocket::connected, this, &Socks5UdpAssociation::sendGreeting);
    connect(control, &QTcpSocket::readyRead, this, &Socks5UdpAssociation::onReadyRead);
    connect(control, &QTcpSocket::disconnected, this, [this] {
        fail(stage == Stage::Established ? QStringLiteral("proxy closed the UDP association")
                                         : QStringLiteral("proxy closed the connection during handshake"));
    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(control, &QTcpSocket::errorOccurred, this, [this] { fail(control->errorString()); });
#else
    connect(control, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this,
            [this] { fail(control->errorString()); });
#endif

    timeout.start(HANDSHAKE_TIMEOUT_MS);
    control->connectToHost(proxy.host, proxy.port);
}

void Socks5UdpAssociation::close()
{
    timeout.stop();
    releaseControlSocket();

    buffer.clear();
    stage = Stage::Idle;
    relay = QHostAddress();
    relayPortNumber = 0;
}

void Socks5UdpAssociation::releaseControlSocket()
{
    if (!control)
        return;

    control->disconnect(this);
    control->deleteLater();
    control = nullptr;
}

void Socks5UdpAssociation::sendGreeting()
{
    QByteArray methods(1, METHOD_NONE);
    if (!proxy.username.isEmpty())
        methods.append(METHOD_USERPASS);

    QByteArray greeting;
    greeting.append(SOCKS_VERSION);
    greeting.append(static_cast<char>(methods.size()));
    greeting.append(methods);

    control->write(greeting);
}

void Socks5UdpAssociation::sendCredentials()
{
    const QByteArray user = proxy.username.toUtf8();
    const QByteArray pass = proxy.password.toUtf8();

    if (user.size() > 255 || pass.size() > 255) {
        fail(QStringLiteral("SOCKS5 credentials exceed 255 bytes"));
        return;
    }

    QByteArray request;
    request.append(AUTH_VERSION);
    request.append(static_cast<char>(user.size()));
    request.append(user);
    request.append(static_cast<char>(pass.size()));
    request.append(pass);

    stage = Stage::AwaitingAuthReply;
    control->write(request);
}

void Socks5UdpAssociation::sendAssociateRequest()
{
    QByteArray request;
    request.append(SOCKS_VERSION);
    request.append(CMD_UDP_ASSOCIATE);
    request.append(char(0));
    appendAddress(request, QHostAddress(QHostAddress::AnyIPv4), 0);

    stage = Stage::AwaitingAssociateReply;
    control->write(request);
}

void Socks5UdpAssociation::onReadyRead()
{
    buffer.append(control->readAll());

    bool progressed = true;
    while (progressed) {
        switch (stage) {
        case Stage::AwaitingMethod:
            progressed = consumeMethodReply();
            break;
        case Stage::AwaitingAuthReply:
            progressed = consumeAuthReply();
            break;
        case Stage::AwaitingAssociateReply:
            progressed = consumeAssociateReply();
            break;
        default:
            progressed = false;
            break;
        }
    }
}

bool Socks5UdpAssociation::consumeMethodReply()
{
    if (buffer.size() < 2)
        return false;

    const char method = buffer.at(1);
    buffer.remove(0, 2);

    if (method == METHOD_UNACCEPTABLE) {
        fail(QStringLiteral("proxy rejected all offered authentication methods"));
        return false;
    }

    if (method == METHOD_USERPASS) {
        sendCredentials();
        return true;
    }

    if (method != METHOD_NONE) {
        fail(QStringLiteral("proxy selected unsupported authentication method %1").arg(int(method)));
        return false;
    }

    sendAssociateRequest();
    return true;
}

bool Socks5UdpAssociation::consumeAuthReply()
{
    if (buffer.size() < 2)
        return false;

    const char status = buffer.at(1);
    buffer.remove(0, 2);

    if (status != 0) {
        fail(QStringLiteral("proxy rejected the supplied credentials"));
        return false;
    }

    sendAssociateRequest();
    return true;
}

bool Socks5UdpAssociation::consumeAssociateReply()
{
    if (buffer.size() < 4)
        return false;

    const quint8 reply = static_cast<quint8>(buffer.at(1));
    if (reply != REPLY_SUCCEEDED) {
        fail(QStringLiteral("UDP ASSOCIATE failed: %1").arg(replyErrorString(reply)));
        return false;
    }

    const char atyp = buffer.at(3);
    if (atyp != ATYP_IPV4 && atyp != ATYP_IPV6) {
        fail(QStringLiteral("proxy returned an unsupported relay address type %1").arg(int(atyp)));
        return false;
    }

    const int addressSize = atyp == ATYP_IPV4 ? 4 : 16;
    const int replySize = 4 + addressSize + 2;
    if (buffer.size() < replySize)
        return false;

    const char *address = buffer.constData() + 4;
    if (atyp == ATYP_IPV4) {
        relay = QHostAddress(qFromBigEndian<quint32>(address));
    } else {
        Q_IPV6ADDR raw;
        std::copy(address, address + 16, raw.c);
        relay = QHostAddress(raw);
    }
    relayPortNumber = qFromBigEndian<quint16>(address + addressSize);
    buffer.remove(0, replySize);

    // a wildcard bind address means "reach me at the address you already used"
    if (relay.isNull() || relay == QHostAddress(QHostAddress::AnyIPv4) || relay == QHostAddress(QHostAddress::AnyIPv6))
        relay = control->peerAddress();

    if (relayPortNumber == 0) {
        fail(QStringLiteral("proxy returned an invalid relay port"));
        return false;
    }

    stage = Stage::Established;
    timeout.stop();

    qCInfo(LogVoice) << "SOCKS5 UDP relay established at" << relay.toString() << ":" << relayPortNumber;

    emit established();
    return false;
}

void Socks5UdpAssociation::fail(const QString &error)
{
    if (stage == Stage::Failed)
        return;

    stage = Stage::Failed;
    timeout.stop();

    qCWarning(LogVoice) << "SOCKS5 UDP association failed:" << error;

    releaseControlSocket();

    // listeners destroy this object on failure, and we may be inside the socket's own signal
    QMetaObject::invokeMethod(this, [this, error] { emit failed(error); }, Qt::QueuedConnection);
}

int Socks5UdpAssociation::writeRelayHeader(std::span<uint8_t> out, const QHostAddress &destination, quint16 destinationPort)
{
    const int size = DATAGRAM_PREFIX_SIZE + addressFieldSize(destination);
    if (static_cast<int>(out.size()) < size)
        return -1;

    std::memset(out.data(), 0, DATAGRAM_PREFIX_SIZE);
    writeAddress(out.data() + DATAGRAM_PREFIX_SIZE, destination, destinationPort);
    return size;
}

bool Socks5UdpAssociation::wrap(PacketBuffer &packet, std::span<const uint8_t> relayHeader)
{
    if (!packet.growFront(static_cast<int>(relayHeader.size())))
        return false;
    std::memcpy(packet.data(), relayHeader.data(), relayHeader.size());
    return true;
}

bool Socks5UdpAssociation::unwrap(PacketBuffer &packet)
{
    const char *datagram = reinterpret_cast<const char *>(packet.data());

    // fragmented relays (FRAG != 0) are not supported; Discord never fragments
    if (packet.size() < DATAGRAM_PREFIX_SIZE + 1 || datagram[2] != 0)
        return false;

    const int tail = addressFieldLength(datagram, packet.size(), DATAGRAM_PREFIX_SIZE);
    if (tail < 0)
        return false;

    return packet.dropFront(DATAGRAM_PREFIX_SIZE + tail);
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
