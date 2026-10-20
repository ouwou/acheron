// A real VoiceClient, with its gateway socket, UDP transport and cipher, against a voice server
// that lives in this file: a WebSocket and a UDP socket on the loopback interface that speak
// just enough of Discord's side to bring a connection up, send it video and take video from it.

#include "Core/Audio/VoiceConnection.hpp"
#include "Core/Streams/StreamConnection.hpp"
#include "Discord/Voice/DaveSession.hpp"
#include "Discord/Voice/Rtcp.hpp"
#include "Discord/Voice/RtpPacket.hpp"
#include "Discord/Voice/TransportFeedback.hpp"
#include "Discord/Voice/VideoDepacketizer.hpp"
#include "Discord/Voice/VideoSendStream.hpp"
#include "Discord/Voice/VoiceClient.hpp"
#include "Discord/Voice/VoiceEncryption.hpp"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUdpSocket>
#include <QtEndian>

#include <array>
#include <memory>
#include <utility>
#include <vector>

using namespace Acheron;
using namespace Acheron::Discord::Voice;

namespace {

constexpr int VIDEO_PAYLOAD_TYPE = 101;
constexpr int RTX_PAYLOAD_TYPE = 102;
constexpr int H264_PAYLOAD_TYPE = 103;
constexpr int H264_RTX_PAYLOAD_TYPE = 104;
constexpr int H265_PAYLOAD_TYPE = 105;
constexpr int AV1_PAYLOAD_TYPE = 107;
constexpr uint8_t TRANSPORT_SEQUENCE_EXTENSION = 5;
constexpr int PACKETS_PER_FRAME = 3;

constexpr quint64 ALICE = 1001;
constexpr quint32 ALICE_SSRC = 6000;
constexpr quint32 ALICE_RTX_SSRC = 6001;
constexpr quint64 BOB = 1002;
constexpr quint32 BOB_SSRC = 7000;
constexpr quint32 BOB_RTX_SSRC = 7001;

// What the server gives the client to send on. Told of no video streams, the client takes the
// two SSRCs after its audio one.
constexpr quint32 OWN_AUDIO_SSRC = 777;
constexpr quint32 OWN_VIDEO_SSRC = OWN_AUDIO_SSRC + 1;
constexpr quint32 OWN_RTX_SSRC = OWN_AUDIO_SSRC + 2;
constexpr quint32 SERVER_SSRC = 1;

using Bytes = std::vector<uint8_t>;

// One RTP packet the client sent, opened.
struct RtpSeen
{
    RtpHeader header;
    int size = 0;
    Bytes extensions;
    Bytes payload;
};

// When the packet says it left, in seconds: its abs-send-time extension, 24 bits of 6.18
// fixed point.
double sendTimeOf(const RtpSeen &packet)
{
    const std::span<const uint8_t> time = findRtpExtension(packet.extensions, VideoSendStream::ABS_SEND_TIME_EXTENSION);
    return time.size() == 3 ? ((time[0] << 16) | (time[1] << 8) | time[2]) / 262144.0 : -1.0;
}

Bytes pictureLoss(quint32 mediaSsrc)
{
    Bytes packet(12);
    writePli(packet, SERVER_SSRC, mediaSsrc);
    return packet;
}

Bytes nack(quint32 mediaSsrc, quint16 sequence)
{
    Bytes packet(16);
    const quint16 sequences[] = { sequence };
    writeNack(packet, SERVER_SSRC, mediaSsrc, sequences);
    return packet;
}

// RFC 5104, 4.3.1: the stream is named in an entry, not in the header.
Bytes fullIntraRequest(quint32 mediaSsrc)
{
    Bytes packet = { 0x84, 206, 0, 4, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 };
    qToBigEndian(mediaSsrc, packet.data() + 12);
    return packet;
}

// REMB, with the rate as an 18-bit mantissa under a 6-bit exponent.
Bytes receiverEstimate(quint64 bitsPerSecond)
{
    int exponent = 0;
    while (bitsPerSecond >> exponent > 0x3FFFF)
        exponent++;
    const quint32 mantissa = static_cast<quint32>(bitsPerSecond >> exponent);
    Bytes packet = { 0x8F, 206, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 'R', 'E', 'M', 'B', 1, static_cast<uint8_t>((exponent << 2) | (mantissa >> 16)), static_cast<uint8_t>(mantissa >> 8), static_cast<uint8_t>(mantissa), 0, 0, 0, 0 };
    qToBigEndian(SERVER_SSRC, packet.data() + 4);
    return packet;
}

// Discord's request to receive a sender's video afresh: an application packet named
// 58 53 49 44, with one of that sender's SSRCs where the packet's own sender would be.
Bytes videoReset(quint32 ssrc)
{
    Bytes packet = { 0x80, 204, 0, 2, 0, 0, 0, 0, 0x58, 0x53, 0x49, 0x44 };
    qToBigEndian(ssrc, packet.data() + 4);
    return packet;
}

// Discord's count of who receives a stream: an application packet whose name bytes are
// 30 53 49 44, with an entry of twelve bytes for every receiver.
Bytes receiverCount(quint32 ssrc, int receivers)
{
    Bytes packet = { static_cast<uint8_t>(0x80 | receivers), 204, 0, static_cast<uint8_t>(2 + receivers * 3), 0, 0, 0, 0, 0x30, 0x53, 0x49, 0x44 };
    qToBigEndian(ssrc, packet.data() + 4);
    packet.resize(packet.size() + static_cast<size_t>(receivers) * 12, 0x11);
    return packet;
}

struct NackSeen
{
    quint32 mediaSsrc = 0;
    quint16 sequence = 0;

    bool operator==(const NackSeen &) const = default;
};

class FakeVoiceServer : public QObject
{
public:
    FakeVoiceServer()
    {
        gateway.listen(QHostAddress::LocalHost);
        // Spelled out, because Qt 5 takes the bare enumerator for a port number.
        media.bind(QHostAddress(QHostAddress::LocalHost));
        connect(&gateway, &QTcpServer::newConnection, this, &FakeVoiceServer::onGatewayConnection);
        connect(&media, &QUdpSocket::readyRead, this, &FakeVoiceServer::onDatagrams);
    }

    [[nodiscard]] QString endpoint() const { return QStringLiteral("127.0.0.1:%1").arg(gateway.serverPort()); }
    [[nodiscard]] quint16 gatewayPort() const { return gateway.serverPort(); }

    [[nodiscard]] QList<QJsonObject> received(int opcode) const { return receivedByOpcode.value(opcode); }
    // The same messages as the client wrote them, for what depends on the order of their keys.
    [[nodiscard]] QList<QByteArray> receivedText(int opcode) const { return textByOpcode.value(opcode); }

    void send(int opcode, const QJsonObject &data)
    {
        const QByteArray text = QJsonDocument(QJsonObject{ { "op", opcode }, { "d", data } }).toJson(QJsonDocument::Compact);
        QByteArray frame;
        frame.append(char(0x81));
        if (text.size() < 126) {
            frame.append(char(text.size()));
        } else {
            frame.append(char(126));
            frame.append(char(text.size() >> 8));
            frame.append(char(text.size() & 0xFF));
        }
        frame.append(text);
        socket->write(frame);
    }

    // Closes the gateway socket with a code the client does not resume after.
    void closeWith(quint16 code)
    {
        QByteArray frame = QByteArray::fromHex("8802");
        frame.append(char(code >> 8));
        frame.append(char(code & 0xFF));
        socket->write(frame);
        socket->disconnectFromHost();
    }

    [[nodiscard]] int gatewayConnections() const { return static_cast<int>(gatewayPeerPorts.size()); }

    void announceVideo(quint64 userId, quint32 ssrc, quint32 rtxSsrc)
    {
        const QJsonObject stream{ { "type", "video" }, { "rid", "100" }, { "ssrc", qint64(ssrc) }, { "rtx_ssrc", qint64(rtxSsrc) }, { "active", true }, { "quality", 100 } };
        send(12, QJsonObject{ { "user_id", QString::number(userId) }, { "audio_ssrc", qint64(ssrc - 1000) }, { "video_ssrc", qint64(ssrc) }, { "streams", QJsonArray{ stream } } });
    }

    // A VP8 payload the depacketizer takes: the descriptor says whether a picture starts here,
    // and the frame header's first bit whether it is a key frame.
    static std::vector<uint8_t> payload(bool first, bool keyframe, uint8_t tag)
    {
        return { static_cast<uint8_t>(first ? 0x10 : 0x00), static_cast<uint8_t>(keyframe ? 0x00 : 0x01), tag, tag };
    }

    void sendPacket(quint32 ssrc, int payloadType, quint16 sequence, quint32 timestamp, bool marker, std::span<const uint8_t> body)
    {
        RtpHeader header;
        header.extension = true;
        header.marker = marker;
        header.payloadType = static_cast<uint8_t>(payloadType);
        header.sequence = sequence;
        header.timestamp = timestamp;
        header.ssrc = ssrc;

        uint8_t numbered[2];
        qToBigEndian<quint16>(nextTransportSequence++, numbered);
        const RtpExtension elements[] = { { TRANSPORT_SEQUENCE_EXTENSION, numbered } };
        uint8_t extensions[8];
        const int extensionSize = writeRtpExtensions(elements, extensions);

        PacketBuffer packet;
        const int clearSize = writeRtpHeader(header, { packet.data(), static_cast<size_t>(packet.tailroom()) }, extensionSize / 4);
        packet.resize(clearSize);
        packet.append({ extensions, static_cast<size_t>(extensionSize) });
        packet.append(body);
        cipher->seal(packet, clearSize);
        media.writeDatagram(reinterpret_cast<const char *>(packet.data()), packet.size(), clientAddress, clientPort);
    }

    // Frame number `index` of a sender whose numbering starts at `base`, leaving out packet `skip`.
    void sendFrame(quint32 ssrc, int index, quint16 base, bool keyframe, int skip = -1)
    {
        for (int i = 0; i < PACKETS_PER_FRAME; i++) {
            if (i == skip)
                continue;
            const quint16 sequence = static_cast<quint16>(base + index * PACKETS_PER_FRAME + i);
            const std::vector<uint8_t> body = payload(i == 0, keyframe, static_cast<uint8_t>(sequence));
            sendPacket(ssrc, VIDEO_PAYLOAD_TYPE, sequence, 3000u * static_cast<quint32>(index), i == PACKETS_PER_FRAME - 1, body);
        }
    }

    void retransmit(quint32 rtxSsrc, quint16 rtxSequence, int index, quint16 base, bool keyframe, int packetOfFrame)
    {
        const quint16 original = static_cast<quint16>(base + index * PACKETS_PER_FRAME + packetOfFrame);
        std::vector<uint8_t> body(2);
        qToBigEndian<quint16>(original, body.data());
        const std::vector<uint8_t> repaired = payload(packetOfFrame == 0, keyframe, static_cast<uint8_t>(original));
        body.insert(body.end(), repaired.begin(), repaired.end());
        sendPacket(rtxSsrc, RTX_PAYLOAD_TYPE, rtxSequence, 3000u * static_cast<quint32>(index), packetOfFrame == PACKETS_PER_FRAME - 1, body);
    }

    [[nodiscard]] quint16 transportSequencesSent() const { return nextTransportSequence; }

    // A compound packet as the server sends them: a receiver report, then whatever else.
    void sendFeedback(std::initializer_list<Bytes> parts, std::span<const ReceptionReport> reports = {})
    {
        PacketBuffer packet;
        packet.resize(8 + 24 * static_cast<int>(reports.size()));
        writeReceiverReport(packet.bytes(), SERVER_SSRC, reports);
        for (const Bytes &part : parts)
            packet.append(part);
        cipher->seal(packet, RTCP_CLEAR_SIZE);
        media.writeDatagram(reinterpret_cast<const char *>(packet.data()), packet.size(), clientAddress, clientPort);
    }

    // What READY and SESSION_DESCRIPTION say, for a test to set before the client gets there.
    QJsonArray readyStreams;
    QString videoCodec = QStringLiteral("VP8");
    int daveProtocolVersion = 0;
    // Closes every connection as it identifies, so that none comes up.
    bool dropOnIdentify = false;

    QList<RtpSeen> rtp;
    QList<ReceivedRtcp::SenderReport> senderReports;
    QList<NackSeen> nacks;
    QList<quint32> keyframeRequests;
    QList<quint32> receptionReports;
    // One past the highest of the server's own packet numbers the client has reported on.
    int acknowledgedUpTo = 0;
    int undecryptable = 0;
    // Where the gateway's connections and the media datagrams came from.
    QList<quint16> gatewayPeerPorts;
    QSet<quint16> mediaSenderPorts;

private:
    void onGatewayConnection()
    {
        socket = gateway.nextPendingConnection();
        upgraded = false;
        incoming.clear();
        cipher.reset();
        gatewayPeerPorts.append(socket->peerPort());
        connect(socket, &QTcpSocket::readyRead, this, &FakeVoiceServer::onGatewayBytes);
    }

    void onGatewayBytes()
    {
        incoming.append(socket->readAll());

        if (!upgraded) {
            const int headersEnd = incoming.indexOf("\r\n\r\n");
            if (headersEnd < 0)
                return;
            QByteArray key;
            for (const QByteArray &line : incoming.left(headersEnd).split('\n')) {
                if (line.toLower().startsWith("sec-websocket-key:"))
                    key = line.mid(line.indexOf(':') + 1).trimmed();
            }
            incoming.remove(0, headersEnd + 4);
            const QByteArray accept = QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
            socket->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
            upgraded = true;
            send(8, QJsonObject{ { "v", 9 }, { "heartbeat_interval", 5000 } });
        }

        for (;;) {
            if (incoming.size() < 2)
                return;
            const auto *bytes = reinterpret_cast<const uint8_t *>(incoming.constData());
            const int opcode = bytes[0] & 0x0F;
            const bool masked = bytes[1] & 0x80;
            qint64 length = bytes[1] & 0x7F;
            int offset = 2;
            if (length == 126) {
                if (incoming.size() < 4)
                    return;
                length = qFromBigEndian<quint16>(bytes + 2);
                offset = 4;
            }
            const int maskSize = masked ? 4 : 0;
            if (incoming.size() < offset + maskSize + length)
                return;

            QByteArray body = incoming.mid(offset + maskSize, static_cast<int>(length));
            if (masked) {
                for (int i = 0; i < body.size(); i++)
                    body[i] = static_cast<char>(body[i] ^ incoming[offset + (i % 4)]);
            }
            incoming.remove(0, offset + maskSize + static_cast<int>(length));

            constexpr int TEXT = 1;
            constexpr int CLOSE = 8;
            if (opcode == CLOSE) {
                socket->write(QByteArray::fromHex("8800"));
                socket->disconnectFromHost();
                return;
            }
            if (opcode == TEXT)
                onGatewayMessage(body);
        }
    }

    void onGatewayMessage(const QByteArray &text)
    {
        constexpr int IDENTIFY = 0;
        constexpr int SELECT_PROTOCOL = 1;
        constexpr int HEARTBEAT = 3;

        const QJsonObject message = QJsonDocument::fromJson(text).object();
        const int opcode = message["op"].toInt();
        const QJsonObject data = message["d"].toObject();
        receivedByOpcode[opcode].append(data);
        textByOpcode[opcode].append(text);

        const QString mode = encryptionModeToString(EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE);
        if (opcode == IDENTIFY && dropOnIdentify) {
            closeWith(4014);
        } else if (opcode == IDENTIFY) {
            send(2, QJsonObject{ { "ssrc", 777 }, { "ip", "127.0.0.1" }, { "port", media.localPort() }, { "modes", QJsonArray{ mode } }, { "streams", readyStreams } });
        } else if (opcode == SELECT_PROTOCOL) {
            const QByteArray key(32, char(0x42));
            QJsonArray keyNumbers;
            for (char byte : key)
                keyNumbers.append(static_cast<uint8_t>(byte));
            cipher = std::make_unique<VoiceEncryption>(EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE, key);
            send(4, QJsonObject{ { "mode", mode }, { "secret_key", keyNumbers }, { "audio_codec", "opus" }, { "video_codec", videoCodec }, { "dave_protocol_version", daveProtocolVersion } });
        } else if (opcode == HEARTBEAT) {
            send(6, QJsonObject{ { "t", data["t"] } });
        }
    }

    void onDatagrams()
    {
        while (media.hasPendingDatagrams()) {
            QByteArray datagram(static_cast<int>(media.pendingDatagramSize()), Qt::Uninitialized);
            QHostAddress from;
            quint16 fromPort = 0;
            media.readDatagram(datagram.data(), datagram.size(), &from, &fromPort);
            mediaSenderPorts.insert(fromPort);
            const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t *>(datagram.constData()), static_cast<size_t>(datagram.size()));

            constexpr int IP_DISCOVERY_SIZE = 74;
            if (!cipher && datagram.size() == IP_DISCOVERY_SIZE) {
                clientAddress = from;
                clientPort = fromPort;
                QByteArray reply(IP_DISCOVERY_SIZE, '\0');
                qToBigEndian<quint16>(2, reply.data());
                qToBigEndian<quint16>(70, reply.data() + 2);
                memcpy(reply.data() + 4, datagram.constData() + 4, 4);
                memcpy(reply.data() + 8, "127.0.0.1", 9);
                qToBigEndian<quint16>(fromPort, reply.data() + 72);
                media.writeDatagram(reply, from, fromPort);
                continue;
            }
            if (cipher && classifyPacket(bytes) == PacketKind::Rtcp)
                onFeedback(bytes);
            else if (cipher && classifyPacket(bytes) == PacketKind::Rtp)
                onMedia(bytes);
        }
    }

    void onMedia(std::span<const uint8_t> sealed)
    {
        const std::optional<ParsedRtp> parsed = parseRtp(sealed);
        PacketBuffer packet;
        packet.append(sealed);
        if (!parsed || !cipher->open(packet, parsed->clearSize) || packet.size() < parsed->clearSize + parsed->extensionSize) {
            undecryptable++;
            return;
        }

        RtpSeen seen;
        seen.header = parsed->header;
        seen.size = packet.size();
        const std::span<const uint8_t> body = packet.bytes().subspan(static_cast<size_t>(parsed->clearSize));
        seen.extensions.assign(body.begin(), body.begin() + parsed->extensionSize);
        seen.payload.assign(body.begin() + parsed->extensionSize, body.end());
        rtp.append(seen);
    }

    void onFeedback(std::span<const uint8_t> sealed)
    {
        PacketBuffer packet;
        packet.append(sealed);
        if (!cipher->open(packet, RTCP_CLEAR_SIZE)) {
            undecryptable++;
            return;
        }

        constexpr int RECEIVER_REPORT = 201;
        constexpr int TRANSPORT_FEEDBACK = 205;
        constexpr int PAYLOAD_FEEDBACK = 206;
        constexpr int GENERIC_NACK = 1;
        constexpr int TRANSPORT_WIDE = 15;
        constexpr int PICTURE_LOSS = 1;

        const std::span<const uint8_t> compound = packet.bytes();
        for (const ReceivedRtcp::SenderReport &report : parseRtcp(compound).senderReports)
            senderReports.append(report);

        size_t offset = 0;
        while (offset + 4 <= compound.size()) {
            const uint8_t *part = compound.data() + offset;
            const int format = part[0] & 0x1F;
            const int type = part[1];
            const size_t size = (static_cast<size_t>(qFromBigEndian<quint16>(part + 2)) + 1) * 4;
            if (offset + size > compound.size())
                return;

            if (type == RECEIVER_REPORT) {
                for (int block = 0; block < format; block++)
                    receptionReports.append(qFromBigEndian<quint32>(part + 8 + block * 24));
            } else if (type == TRANSPORT_FEEDBACK && format == GENERIC_NACK) {
                const quint32 mediaSsrc = qFromBigEndian<quint32>(part + 8);
                for (size_t item = 12; item + 4 <= size; item += 4) {
                    const quint16 first = qFromBigEndian<quint16>(part + item);
                    const quint16 following = qFromBigEndian<quint16>(part + item + 2);
                    nacks.append({ mediaSsrc, first });
                    for (int bit = 0; bit < 16; bit++) {
                        if (following & (1 << bit))
                            nacks.append({ mediaSsrc, static_cast<quint16>(first + bit + 1) });
                    }
                }
            } else if (type == TRANSPORT_FEEDBACK && format == TRANSPORT_WIDE) {
                const int base = qFromBigEndian<quint16>(part + 12);
                const int count = qFromBigEndian<quint16>(part + 14);
                acknowledgedUpTo = qMax(acknowledgedUpTo, base + count);
            } else if (type == PAYLOAD_FEEDBACK && format == PICTURE_LOSS) {
                keyframeRequests.append(qFromBigEndian<quint32>(part + 8));
            }
            offset += size;
        }
    }

    QTcpServer gateway;
    QTcpSocket *socket = nullptr;
    QByteArray incoming;
    bool upgraded = false;
    QHash<int, QList<QJsonObject>> receivedByOpcode;
    QHash<int, QList<QByteArray>> textByOpcode;

    QUdpSocket media;
    QHostAddress clientAddress;
    quint16 clientPort = 0;
    std::unique_ptr<VoiceEncryption> cipher;
    quint16 nextTransportSequence = 0;
};

// A SOCKS5 proxy without authentication that does for an RTC connection what an account's proxy
// does: CONNECT for the gateway's WebSocket and UDP ASSOCIATE for the media, relaying IPv4
// datagrams both ways. It notes what it carried.
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

    [[nodiscard]] Core::ProxyConfig config() const
    {
        Core::ProxyConfig proxy;
        proxy.type = Core::ProxyConfig::Type::Socks5;
        proxy.host = QStringLiteral("127.0.0.1");
        proxy.port = control.serverPort();
        return proxy;
    }

    [[nodiscard]] quint16 relayPort() const { return relay.localPort(); }

    // The ports CONNECT was asked for, and the local ports of the connections it made for them.
    QList<quint16> connectedTo;
    QList<quint16> onwardPorts;
    bool associated = false;
    int datagramsToServer = 0;
    int datagramsToClient = 0;

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
            relay.writeDatagram(datagram.mid(RELAY_HEADER_SIZE), QHostAddress(QHostAddress::LocalHost), destination);
            datagramsToServer++;
        }
    }

    QTcpServer control;
    QUdpSocket relay;
    quint16 clientPort = 0;
    QSet<quint16> destinations;
};

struct FrameSeen
{
    quint64 userId = 0;
    quint32 timestamp = 0;
    bool keyframe = false;
};

// A frame for the client to send. The VP8 packetizer does not look inside what it cuts up.
EncodedVideoFrame videoFrame(size_t size, bool keyframe, quint32 timestamp)
{
    EncodedVideoFrame frame;
    frame.data.resize(size);
    for (size_t i = 0; i < size; i++)
        frame.data[i] = static_cast<uint8_t>(i * 7 + 3);
    frame.rtpTimestamp = timestamp;
    frame.keyframe = keyframe;
    return frame;
}

EncodedVideoFrame h264Keyframe(quint32 timestamp)
{
    EncodedVideoFrame frame;
    frame.data = { 0, 0, 0, 1, 0x67, 0x42, 0x11, 0x22, 0, 0, 0, 1, 0x68, 0x33, 0x44, 0, 0, 0, 1, 0x65, 0x88 };
    frame.data.resize(frame.data.size() + 3000, 0x5A);
    frame.rtpTimestamp = timestamp;
    frame.keyframe = true;
    return frame;
}

// VPS, SPS and PPS, then the first slice segment of an IDR picture.
EncodedVideoFrame h265Keyframe(quint32 timestamp)
{
    EncodedVideoFrame frame;
    frame.data = { 0, 0, 0, 1, 0x40, 0x01, 0x11, 0, 0, 0, 1, 0x42, 0x01, 0x22, 0, 0, 0, 1, 0x44, 0x01, 0x33, 0, 0, 0, 1, 0x26, 0x01, 0x88 };
    frame.data.resize(frame.data.size() + 3000, 0x5A);
    frame.rtpTimestamp = timestamp;
    frame.keyframe = true;
    return frame;
}

constexpr std::array<uint8_t, 5> AV1_SEQUENCE_HEADER = { 0x0A, 3, 0x11, 0x22, 0x33 };
constexpr size_t AV1_PICTURE_SIZE = 3000;

// One temporal unit as an encoder writes it, every OBU with its size: a temporal delimiter, the
// sequence header if it is a key frame, and the frame.
EncodedVideoFrame av1Frame(bool keyframe, quint32 timestamp)
{
    EncodedVideoFrame frame;
    frame.data = { 0x12, 0 };
    if (keyframe)
        frame.data.insert(frame.data.end(), AV1_SEQUENCE_HEADER.begin(), AV1_SEQUENCE_HEADER.end());
    // The frame's header, and 3000 as LEB128.
    frame.data.insert(frame.data.end(), { 0x32, 0xB8, 0x17 });
    frame.data.resize(frame.data.size() + AV1_PICTURE_SIZE, 0x5A);
    frame.rtpTimestamp = timestamp;
    frame.keyframe = keyframe;
    return frame;
}

// The same as it is assembled from its packets: the temporal delimiter is not sent, and the
// last OBU has no size field.
Bytes av1AsAssembled(bool keyframe)
{
    Bytes frame;
    if (keyframe)
        frame.assign(AV1_SEQUENCE_HEADER.begin(), AV1_SEQUENCE_HEADER.end());
    frame.push_back(0x30);
    frame.resize(frame.size() + AV1_PICTURE_SIZE, 0x5A);
    return frame;
}

// A voice connection that offers VP8 and has reached the connected state. One that sends video
// offers to encode it, and H264, H265 and AV1 besides. It can be a stream's connection as its
// owner has it instead, with the sound that is shared or without.
struct Call
{
    enum class Kind {
        Voice,
        VoiceWithSecondLayer,
        Stream,
        StreamWithSound,
    };

    explicit Call(bool sendsVideo = false, Kind kind = Kind::Voice, const Core::ProxyConfig &proxy = {})
    {
        ConnectionParams params;
        params.endpoint = server.endpoint();
        params.proxy = proxy;
        params.gatewayScheme = QStringLiteral("ws");
        params.token = QStringLiteral("token");
        params.serverId = Core::Snowflake(1);
        params.channelId = Core::Snowflake(2);
        params.userId = Core::Snowflake(3);
        params.sessionId = QStringLiteral("session");
        params.video = true;
        if (kind == Kind::Voice || kind == Kind::VoiceWithSecondLayer)
            params.streams = cameraStreams(kind == Kind::VoiceWithSecondLayer);
        params.sendsAudio = kind == Kind::StreamWithSound;
        params.streamContext = kind == Kind::Stream || kind == Kind::StreamWithSound;

        Codec vp8;
        vp8.name = QStringLiteral("VP8");
        vp8.type = QStringLiteral("video");
        vp8.priority = 1000;
        vp8.payloadType = VIDEO_PAYLOAD_TYPE;
        vp8.rtxPayloadType = RTX_PAYLOAD_TYPE;
        vp8.encode = sendsVideo;
        vp8.decode = true;
        params.videoCodecs = { vp8 };
        if (sendsVideo) {
            const std::pair<QString, int> sentOnly[] = { { QStringLiteral("H264"), H264_PAYLOAD_TYPE }, { QStringLiteral("H265"), H265_PAYLOAD_TYPE }, { QStringLiteral("AV1"), AV1_PAYLOAD_TYPE } };
            for (const auto &[name, payloadType] : sentOnly) {
                Codec codec;
                codec.name = name;
                codec.type = QStringLiteral("video");
                codec.priority = 2000;
                codec.payloadType = payloadType;
                codec.rtxPayloadType = payloadType + 1;
                codec.encode = true;
                codec.decode = false;
                params.videoCodecs.append(codec);
            }
        }

        client = std::make_unique<VoiceClient>(params);
        client->setVideoFrameHandler([this](Core::Snowflake userId, VideoCodec, EncodedVideoFrame frame) {
            frames.append({ static_cast<quint64>(userId), frame.rtpTimestamp, frame.keyframe });
        });
        QObject::connect(client.get(), &VoiceClient::remoteVideoChanged, client.get(), [this](Core::Snowflake userId, bool active) {
            if (active)
                announced.append(static_cast<quint64>(userId));
        });
        client->start();
    }

    ~Call() { client->stop(); }

    [[nodiscard]] bool isUp() const { return client->state() == VoiceClient::State::Connected; }

    // Alice and Bob both have a camera on, and both are received.
    void receiveAliceAndBob()
    {
        server.announceVideo(ALICE, ALICE_SSRC, ALICE_RTX_SSRC);
        server.announceVideo(BOB, BOB_SSRC, BOB_RTX_SSRC);
        QTRY_COMPARE(announced.size(), 2);

        MediaSinkWants wants;
        wants.any = 100;
        client->setVideoPlan(wants, { ALICE_SSRC, BOB_SSRC });
    }

    [[nodiscard]] int framesFrom(quint64 userId) const
    {
        return static_cast<int>(std::count_if(frames.cbegin(), frames.cend(), [userId](const FrameSeen &frame) { return frame.userId == userId; }));
    }

    void startVideo()
    {
        LocalVideoDescription description;
        description.maxWidth = 1280;
        description.maxHeight = 720;
        description.maxFramerate = 30;
        description.maxBitrate = 3500000;
        client->startVideo(description);
    }

    // The payloads of the packets the server has from the client on one SSRC, put back together.
    [[nodiscard]] Bytes reassembled(VideoCodec codec, quint32 ssrc) const
    {
        std::vector<std::span<const uint8_t>> payloads;
        for (const RtpSeen &packet : server.rtp) {
            if (packet.header.ssrc == ssrc)
                payloads.emplace_back(packet.payload);
        }
        Bytes frame;
        if (!assembleFrame(codec, payloads, frame))
            frame.clear();
        return frame;
    }

    FakeVoiceServer server;
    std::unique_ptr<VoiceClient> client;
    QList<FrameSeen> frames;
    QList<quint64> announced;
};

} // namespace

class TestVoiceClient : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(VoiceEncryption::initialize());
        qRegisterMetaType<Core::StreamConnection::Failure>();
    }

    void connectsAndSaysWhatItCanReceive();
    void aVoiceConnectionDeclaresOneCameraLayer();
    void aVoiceConnectionMayDeclareASecondCameraLayer();
    void usersTheServerNamesAreAnnounced();
    void framesReachTheHandlerUnderTheirSender();
    void aLostPacketIsAskedForAndTakenFromTheRetransmission();
    void aKeyframeRequestGoesToThatSenderAlone();
    void aVideoResetRequestStartsThatSendersVideoOver();
    void everyPacketTheServerNumberedIsReportedBack();

    void theServerNamesTheCodecVideoIsSentWith();
    void nothingIsSaidOfVideoBeforeItHasStarted();
    void startingVideoAnnouncesEveryLayerItHas();
    void eachLayerIsAStreamOfItsOwnUnderOneNumbering();
    void withoutStreamsFromTheServerTheSsrcsFollowTheAudioOne();
    void aFrameArrivesAsPacketsThatReassemble();
    void aFrameLeavesAtThePaceOfTheBitrateItWasEncodedAt();
    void stoppingVideoSaysSoAndSendsNothingMore();
    void aNackBringsThePacketBackOnTheRetransmissionStream();
    void keyframeRequestsReachTheEncoderButNotInABurst();
    void theReceiverCountIsPassedOn();
    void whatViewersWantIsPassedOn();
    void aSenderReportAndStatisticsFollowEverySecond();
    void aWayFoundToFallBehindIsSaidAtOnce();
    void aChangeOfCodecIsTakenUp();
    void aChangeToAv1WaitsForItsKeyframe();
    void encodersFoundLateAreOfferedInASessionUpdate();
    void encodersFoundBeforeTheProtocolIsChosenNeedNoUpdate();
    void aCodecWithdrawnWhileInUseSendsNothingUntilAnotherIsNamed();
    void noVideoGoesOutWhileEndToEndKeysAreMissing();
    void onlyACommitThatGivesSomeoneAKeyWantsAKeyframe();

    void sharedSoundIsAnnouncedOnceAndMarkedAtEveryRun();
    void aStreamWithoutSoundSendsNoAudio();

    void aProxiedConnectionReachesTheServerThroughTheProxyAlone_data();
    void aProxiedConnectionReachesTheServerThroughTheProxyAlone();

    void aStreamConnectionIsMadeWhereTheGatewaySays();
    void aDroppedStreamConnectionIsMadeAgainAfterAPing();
    void aStreamConnectionDroppingEveryTimeIsGivenUpOn();
    void aStreamThatMovesIsFollowed();
    void aStreamConnectionNotUpInTimeFails();
    void aStreamConnectionRefusesAProxyThatCannotCarryIt();
};

void TestVoiceClient::connectsAndSaysWhatItCanReceive()
{
    Call call;
    QTRY_VERIFY(call.isUp());

    const QList<QJsonObject> identify = call.server.received(0);
    QCOMPARE(identify.size(), 1);
    QCOMPARE(identify[0]["server_id"].toString(), QStringLiteral("1"));
    QCOMPARE(identify[0]["video"].toBool(), true);

    const QList<QJsonObject> selectProtocol = call.server.received(1);
    QCOMPARE(selectProtocol.size(), 1);
    const QJsonArray codecs = selectProtocol[0]["codecs"].toArray();
    QCOMPARE(codecs.size(), 2);
    QCOMPARE(codecs[0].toObject()["name"].toString(), QStringLiteral("opus"));
    QCOMPARE(codecs[1].toObject()["name"].toString(), QStringLiteral("VP8"));
    QCOMPARE(codecs[1].toObject()["payload_type"].toInt(), VIDEO_PAYLOAD_TYPE);
    QCOMPARE(codecs[1].toObject()["rtx_payload_type"].toInt(), RTX_PAYLOAD_TYPE);
    QCOMPARE(selectProtocol[0]["data"].toObject()["mode"].toString(), encryptionModeToString(EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE));
}

void TestVoiceClient::aVoiceConnectionDeclaresOneCameraLayer()
{
    Call call;
    QTRY_VERIFY(call.isUp());

    QCOMPARE(call.server.receivedText(0).size(), 1);
    QVERIFY2(call.server.receivedText(0)[0].endsWith(R"("video":true,"streams":[{"type":"video","rid":"100","quality":100}]}})"), call.server.receivedText(0)[0].constData());
    QCOMPARE(call.server.received(0)[0]["streams"].toArray().size(), 1);
}

void TestVoiceClient::aVoiceConnectionMayDeclareASecondCameraLayer()
{
    Call call(false, Call::Kind::VoiceWithSecondLayer);
    QTRY_VERIFY(call.isUp());

    QCOMPARE(call.server.receivedText(0).size(), 1);
    QVERIFY2(call.server.receivedText(0)[0].endsWith(R"("video":true,"streams":[{"type":"video","rid":"100","quality":100},{"type":"video","rid":"50","quality":50}]}})"), call.server.receivedText(0)[0].constData());
}

void TestVoiceClient::usersTheServerNamesAreAnnounced()
{
    Call call;
    QTRY_VERIFY(call.isUp());

    QList<quint64> reached;
    QList<quint64> left;
    QObject::connect(call.client.get(), &VoiceClient::clientReachedServer, call.client.get(), [&reached](Core::Snowflake userId) { reached.append(static_cast<quint64>(userId)); });
    QObject::connect(call.client.get(), &VoiceClient::clientDisconnected, call.client.get(), [&left](Core::Snowflake userId) { left.append(static_cast<quint64>(userId)); });

    // Bob moves from one device to another: off the server, then on it again, by id alone.
    constexpr int CLIENTS_CONNECT = 11;
    constexpr int CLIENT_DISCONNECT = 13;
    call.server.send(CLIENTS_CONNECT, QJsonObject{ { "user_ids", QJsonArray{ QString::number(ALICE), QString::number(BOB) } } });
    call.server.send(CLIENT_DISCONNECT, QJsonObject{ { "user_id", QString::number(BOB) } });
    call.server.send(CLIENTS_CONNECT, QJsonObject{ { "user_ids", QJsonArray{ QString::number(BOB) } } });

    QTRY_COMPARE(reached, (QList<quint64>{ ALICE, BOB, BOB }));
    QCOMPARE(left, QList<quint64>{ BOB });
}

void TestVoiceClient::framesReachTheHandlerUnderTheirSender()
{
    Call call;
    QTRY_VERIFY(call.isUp());
    call.receiveAliceAndBob();

    call.server.sendFrame(ALICE_SSRC, 0, 100, true);
    call.server.sendFrame(BOB_SSRC, 0, 40000, true);
    call.server.sendFrame(ALICE_SSRC, 1, 100, false);
    QTRY_COMPARE(call.frames.size(), 3);

    QCOMPARE(call.framesFrom(ALICE), 2);
    QCOMPARE(call.framesFrom(BOB), 1);
    QVERIFY(call.frames[0].keyframe);
    QCOMPARE(call.server.undecryptable, 0);

    // What the server is told to send follows the plan.
    const QList<QJsonObject> wants = call.server.received(15);
    QVERIFY(!wants.isEmpty());
    QCOMPARE(wants.last()["any"].toInt(), 100);
}

void TestVoiceClient::aLostPacketIsAskedForAndTakenFromTheRetransmission()
{
    Call call;
    QTRY_VERIFY(call.isUp());
    call.receiveAliceAndBob();

    constexpr quint16 BASE = 65530;
    constexpr int LOST_PACKET = 1;
    call.server.sendFrame(ALICE_SSRC, 0, BASE, true);
    call.server.sendFrame(ALICE_SSRC, 1, BASE, false, LOST_PACKET);
    call.server.sendFrame(ALICE_SSRC, 2, BASE, false);
    QTRY_COMPARE(call.frames.size(), 1);

    const NackSeen expected{ ALICE_SSRC, static_cast<quint16>(BASE + PACKETS_PER_FRAME + LOST_PACKET) };
    QTRY_VERIFY(call.server.nacks.contains(expected));
    QVERIFY(std::all_of(call.server.nacks.cbegin(), call.server.nacks.cend(), [&expected](const NackSeen &nack) { return nack == expected; }));

    call.server.retransmit(ALICE_RTX_SSRC, 0, 1, BASE, false, LOST_PACKET);
    QTRY_COMPARE(call.frames.size(), 3);
    QVERIFY(!call.server.keyframeRequests.contains(ALICE_SSRC));
}

void TestVoiceClient::aKeyframeRequestGoesToThatSenderAlone()
{
    Call call;
    QTRY_VERIFY(call.isUp());
    call.receiveAliceAndBob();

    call.server.sendFrame(ALICE_SSRC, 0, 100, true);
    call.server.sendFrame(BOB_SSRC, 0, 40000, true);
    QTRY_COMPARE(call.frames.size(), 2);
    QTest::qWait(100);
    call.server.keyframeRequests.clear();

    // Alice's decoder could not use what it was given.
    call.client->requestVideoKeyframeFrom(Core::Snowflake(ALICE));
    QTRY_VERIFY(call.server.keyframeRequests.contains(ALICE_SSRC));

    // Bob's video carries on from where it was; Alice's waits for her key frame.
    call.server.sendFrame(ALICE_SSRC, 1, 100, false);
    call.server.sendFrame(BOB_SSRC, 1, 40000, false);
    QTRY_COMPARE(call.framesFrom(BOB), 2);
    QTest::qWait(VideoReceiver::KEYFRAME_REQUEST_INTERVAL_MS + 200);
    QCOMPARE(call.framesFrom(ALICE), 1);
    QVERIFY(!call.server.keyframeRequests.contains(BOB_SSRC));

    call.server.sendFrame(ALICE_SSRC, 2, 100, true);
    QTRY_COMPARE(call.framesFrom(ALICE), 2);
}

void TestVoiceClient::aVideoResetRequestStartsThatSendersVideoOver()
{
    Call call;
    QTRY_VERIFY(call.isUp());
    call.receiveAliceAndBob();

    call.server.sendFrame(ALICE_SSRC, 0, 100, true);
    call.server.sendFrame(BOB_SSRC, 0, 40000, true);
    QTRY_COMPARE(call.frames.size(), 2);
    QTest::qWait(100);
    call.server.keyframeRequests.clear();

    // Named by the SSRC of her retransmissions: any of a sender's SSRCs stands for the sender.
    call.server.sendFeedback({ videoReset(ALICE_RTX_SSRC) });
    QTRY_VERIFY(call.server.keyframeRequests.contains(ALICE_SSRC));

    // What builds on pictures from before is of no use to a receiver made anew.
    call.server.sendFrame(ALICE_SSRC, 1, 100, false);
    call.server.sendFrame(BOB_SSRC, 1, 40000, false);
    QTRY_COMPARE(call.framesFrom(BOB), 2);
    QTest::qWait(200);
    QCOMPARE(call.framesFrom(ALICE), 1);
    QVERIFY(!call.server.keyframeRequests.contains(BOB_SSRC));

    call.server.sendFrame(ALICE_SSRC, 2, 100, true);
    QTRY_COMPARE(call.framesFrom(ALICE), 2);

    // One that names nobody known changes nothing.
    call.server.keyframeRequests.clear();
    call.server.sendFeedback({ videoReset(0x7777) });
    call.server.sendFrame(ALICE_SSRC, 3, 100, false);
    QTRY_COMPARE(call.framesFrom(ALICE), 3);
    QVERIFY(call.server.keyframeRequests.isEmpty());
}

void TestVoiceClient::everyPacketTheServerNumberedIsReportedBack()
{
    Call call;
    QTRY_VERIFY(call.isUp());
    call.receiveAliceAndBob();

    call.server.sendFrame(ALICE_SSRC, 0, 100, true);
    call.server.sendFrame(BOB_SSRC, 0, 40000, true);
    // Padding under a payload type nobody plays is numbered like everything else.
    constexpr int PROBE_PAYLOAD_TYPE = 96;
    const std::vector<uint8_t> padding(40, 0);
    call.server.sendPacket(ALICE_RTX_SSRC, PROBE_PAYLOAD_TYPE, 9, 0, false, padding);

    QTRY_COMPARE(call.server.acknowledgedUpTo, static_cast<int>(call.server.transportSequencesSent()));
    QTRY_VERIFY(call.server.receptionReports.contains(ALICE_SSRC) && call.server.receptionReports.contains(BOB_SSRC));
    QCOMPARE(call.server.undecryptable, 0);
}

void TestVoiceClient::theServerNamesTheCodecVideoIsSentWith()
{
    // Offered for decoding alone, the codec the server names is not one to send with.
    Call receiving;
    QTRY_VERIFY(receiving.isUp());
    QCOMPARE(receiving.client->videoSendCodec(), VideoCodec::Unknown);

    VideoCodec codecAtConnected = VideoCodec::Unknown;
    Call call(true);
    QObject::connect(call.client.get(), &VoiceClient::connected, call.client.get(), [&] { codecAtConnected = call.client->videoSendCodec(); });
    QCOMPARE(call.client->videoSendCodec(), VideoCodec::Unknown);
    QTRY_VERIFY(call.isUp());
    QCOMPARE(call.client->videoSendCodec(), VideoCodec::VP8);
    QCOMPARE(codecAtConnected, VideoCodec::VP8);
}

void TestVoiceClient::nothingIsSaidOfVideoBeforeItHasStarted()
{
    Call call(true);

    // Not connected yet: there is nothing to announce video to.
    call.startVideo();
    QTRY_VERIFY(call.isUp());

    QSignalSpy keyframes(call.client.get(), &VoiceClient::videoKeyframeRequested);
    call.client->stopVideo();
    call.client->describeVideo(LocalVideoDescription());
    call.client->sendVideoFrame(videoFrame(3000, true, 0));
    call.server.sendFeedback({ pictureLoss(OWN_VIDEO_SSRC) });
    QTest::qWait(200);

    QVERIFY(call.server.received(12).isEmpty());
    QVERIFY(call.server.rtp.isEmpty());
    QVERIFY(call.server.senderReports.isEmpty());
    QCOMPARE(keyframes.count(), 0);
    QCOMPARE(call.server.undecryptable, 0);
}

void TestVoiceClient::startingVideoAnnouncesEveryLayerItHas()
{
    Call call(true);
    // Two layers, the full one second, and its retransmissions not on the SSRC after its own.
    call.server.readyStreams = QJsonArray{
        QJsonObject{ { "type", "video" }, { "rid", "50" }, { "ssrc", 5002 }, { "rtx_ssrc", 5003 }, { "quality", 50 }, { "active", false } },
        QJsonObject{ { "type", "video" }, { "rid", "100" }, { "ssrc", 5000 }, { "rtx_ssrc", 5005 }, { "quality", 100 }, { "active", false } },
    };
    QSignalSpy layersKnown(call.client.get(), &VoiceClient::videoSendLayersKnown);
    QTRY_VERIFY(call.isUp());
    QCOMPARE(layersKnown.count(), 1);
    QCOMPARE(layersKnown[0][0].value<QList<int>>(), QList<int>({ 50, 100 }));

    // The top names the full layer, with the SSRC after it for its retransmissions whatever
    // READY gave it. Every layer is said to be active, and to be of the same size and rate.
    const QByteArray opening = R"({"op":12,"d":{"audio_ssrc":777,"video_ssrc":5000,"rtx_ssrc":5001,"streams":[)";
    const auto layers = [](const QByteArray &active, const QByteArray &secondLimits, const QByteArray &fullLimits) {
        return QByteArray(R"({"type":"video","rid":"50","ssrc":5002,"active":)") + active + R"(,"quality":50,"rtx_ssrc":5003,)" + secondLimits + R"(},{"type":"video","rid":"100","ssrc":5000,"active":)" + active + R"(,"quality":100,"rtx_ssrc":5005,)" + fullLimits + "}]}}";
    };
    const QByteArray fixedSize = R"("max_framerate":30,"max_resolution":{"type":"fixed","width":1280,"height":720})";

    call.startVideo();
    QTRY_COMPARE(call.server.receivedText(12).size(), 1);
    QCOMPARE(call.server.receivedText(12)[0], opening + layers("true", R"("max_bitrate":3500000,)" + fixedSize, R"("max_bitrate":3500000,)" + fixedSize));
    // Said again, for whoever began to listen after the connection was made.
    QCOMPARE(layersKnown.count(), 2);

    // New limits, this time for whatever size the source has, and less for the second layer.
    LocalVideoDescription source;
    source.maxFramerate = 60;
    source.maxBitrate = 9000000;
    source.secondLayerMaxBitrate = 625000;
    call.client->describeVideo(source);
    QTRY_COMPARE(call.server.receivedText(12).size(), 2);
    const QByteArray sourceSize = R"("max_framerate":60,"max_resolution":{"type":"source","width":0,"height":0})";
    QCOMPARE(call.server.receivedText(12)[1], opening + layers("true", R"("max_bitrate":625000,)" + sourceSize, R"("max_bitrate":9000000,)" + sourceSize));

    // Off: no SSRC at the top, and the layers as they were but inactive.
    call.client->stopVideo();
    QTRY_COMPARE(call.server.receivedText(12).size(), 3);
    QCOMPARE(call.server.receivedText(12)[2], R"({"op":12,"d":{"audio_ssrc":777,"video_ssrc":0,"rtx_ssrc":0,"streams":[)" + layers("false", R"("max_bitrate":625000,)" + sourceSize, R"("max_bitrate":9000000,)" + sourceSize));

    // Already off: there is nothing new to say.
    call.client->stopVideo();
    call.client->describeVideo(source);
    QTest::qWait(100);
    QCOMPARE(call.server.receivedText(12).size(), 3);

    // No speaking message goes with video.
    QVERIFY(call.server.received(5).isEmpty());
}

void TestVoiceClient::eachLayerIsAStreamOfItsOwnUnderOneNumbering()
{
    constexpr quint32 SECOND_SSRC = 5002;
    constexpr quint32 SECOND_RTX_SSRC = 5003;
    constexpr int MEDIA_SINK_WANTS = 15;
    Call call(true);
    call.server.readyStreams = QJsonArray{
        QJsonObject{ { "type", "video" }, { "rid", "100" }, { "ssrc", 5000 }, { "rtx_ssrc", 5001 }, { "quality", 100 }, { "active", false } },
        QJsonObject{ { "type", "video" }, { "rid", "50" }, { "ssrc", int(SECOND_SSRC) }, { "rtx_ssrc", int(SECOND_RTX_SSRC) }, { "quality", 50 }, { "active", false } },
    };
    QTRY_VERIFY(call.isUp());
    call.startVideo();
    QSignalSpy keyframes(call.client.get(), &VoiceClient::videoKeyframeRequested);
    QSignalSpy receivers(call.client.get(), &VoiceClient::videoReceiversChanged);
    QSignalSpy wants(call.client.get(), &VoiceClient::videoWantsReceived);
    QSignalSpy statistics(call.client.get(), &VoiceClient::videoSendStatsUpdated);

    // Three packets of the full picture, one of the second layer, and nothing of a layer
    // the connection does not have.
    call.client->sendVideoFrame(videoFrame(3000, true, 90000));
    call.client->sendVideoFrame(videoFrame(1000, true, 90000), 50);
    call.client->sendVideoFrame(videoFrame(1000, true, 90000), 20);
    QTRY_COMPARE(call.server.rtp.size(), 4);
    QTest::qWait(50);
    QCOMPARE(call.server.rtp.size(), 4);

    int ofTheFull = 0;
    quint16 secondSequence = 0;
    QSet<int> numbers;
    for (const RtpSeen &packet : std::as_const(call.server.rtp)) {
        if (packet.header.ssrc == 5000)
            ofTheFull++;
        else
            secondSequence = packet.header.sequence;
        const std::span<const uint8_t> number = findRtpExtension(packet.extensions, VideoSendStream::TRANSPORT_SEQUENCE_EXTENSION);
        QCOMPARE(number.size(), size_t(2));
        numbers.insert(qFromBigEndian<quint16>(number.data()));
    }
    QCOMPARE(ofTheFull, 3);
    QCOMPARE(call.server.rtp.last().header.ssrc, SECOND_SSRC);
    // One numbering runs through both.
    QCOMPARE(numbers, QSet<int>({ 1, 2, 3, 4 }));

    // What the server says of the second layer is passed on as said of it: who receives it,
    // which brings a key frame for whoever has only now begun to, and a packet to send again,
    // which comes on that layer's own retransmission stream.
    Bytes again(16);
    const quint16 lost[] = { secondSequence };
    again.resize(static_cast<size_t>(writeNack(again, SERVER_SSRC, SECOND_SSRC, lost)));
    call.server.sendFeedback({ receiverCount(SECOND_SSRC, 2), again });
    QTRY_COMPARE(receivers.count(), 1);
    QCOMPARE(receivers[0][0].toInt(), 2);
    QCOMPARE(receivers[0][1].toInt(), 50);
    QTRY_COMPARE(keyframes.count(), 1);
    QCOMPARE(keyframes[0][0].toInt(), 50);
    QTRY_COMPARE(call.server.rtp.size(), 5);
    QCOMPARE(call.server.rtp.last().header.ssrc, SECOND_RTX_SSRC);

    // Each layer hears what is wanted of it: of its own SSRC first, then of any, and a
    // want of nothing counts as a want of all of it.
    call.server.send(MEDIA_SINK_WANTS, QJsonObject{ { "any", 30 }, { QString::number(SECOND_SSRC), 60 }, { "pixelCounts", QJsonObject{ { QString::number(SECOND_SSRC), 147456 } } } });
    QTRY_COMPARE(wants.count(), 2);
    const auto said = [&wants](int index) { return QList<int>({ wants[index][0].toInt(), wants[index][1].toInt(), wants[index][2].toInt() }); };
    QCOMPARE(said(0), QList<int>({ 30, 0, 100 }));
    QCOMPARE(said(1), QList<int>({ 60, 147456, 50 }));
    call.server.send(MEDIA_SINK_WANTS, QJsonObject{ { "any", 0 }, { "5000", 0 }, { QString::number(SECOND_SSRC), 60 } });
    QTRY_COMPARE(wants.count(), 4);
    QCOMPARE(said(2), QList<int>({ 100, 0, 100 }));
    QCOMPARE(said(3), QList<int>({ 60, 0, 50 }));

    // The statistics count both layers together, and each sends a report of its own.
    QTRY_VERIFY_WITH_TIMEOUT(statistics.count() >= 1, 1500);
    const VideoSendStats sent = statistics.last()[0].value<VideoSendStats>();
    QCOMPARE(sent.packets, quint64(4));
    QCOMPARE(sent.frames, quint64(2));
    QCOMPARE(sent.retransmitted, quint64(1));
    QCOMPARE(sent.receivers, -1);
    QSet<quint32> reportedOn;
    for (const ReceivedRtcp::SenderReport &report : std::as_const(call.server.senderReports))
        reportedOn.insert(report.ssrc);
    QCOMPARE(reportedOn, QSet<quint32>({ 5000, SECOND_SSRC }));
}

void TestVoiceClient::withoutStreamsFromTheServerTheSsrcsFollowTheAudioOne()
{
    Call call(true);
    QTRY_VERIFY(call.isUp());

    call.startVideo();
    QTRY_COMPARE(call.server.receivedText(12).size(), 1);
    QCOMPARE(call.server.receivedText(12)[0], QByteArray(R"({"op":12,"d":{"audio_ssrc":777,"video_ssrc":778,"rtx_ssrc":779,"streams":[{"type":"video","rid":"100","ssrc":778,"active":true,"quality":100,"rtx_ssrc":779,"max_bitrate":3500000,"max_framerate":30,"max_resolution":{"type":"fixed","width":1280,"height":720}}]}})"));
}

void TestVoiceClient::aFrameLeavesAtThePaceOfTheBitrateItWasEncodedAt()
{
    Call call(true);
    QTRY_VERIFY(call.isUp());
    call.startVideo();

    // Twelve kilobytes at half a megabit: a burst of 40 ms' worth at once, the rest over more
    // than a tenth of a second. The time is read off the packets' own send times, which say
    // when the pace let each go and nothing of how busy this machine is.
    constexpr int PACKETS = 11;
    const auto timeToLeave = [&](quint32 timestamp) {
        call.server.rtp.clear();
        call.client->sendVideoFrame(videoFrame(12000, true, timestamp));
        [&] { QTRY_COMPARE(call.server.rtp.size(), PACKETS); }();
        if (call.server.rtp.size() != PACKETS)
            return -1.0;
        return (sendTimeOf(call.server.rtp.last()) - sendTimeOf(call.server.rtp.first())) * 1000.0;
    };

    call.client->setVideoBitrate(500000);
    const double slow = timeToLeave(1000);
    QVERIFY2(slow >= 90 && slow < 1000, qPrintable(QString::number(slow)));

    // Until it is told otherwise the pace is that of the most that was announced, which
    // stopping and starting the video brings back: at 3.5 Mbps the whole frame is one burst.
    call.client->stopVideo();
    call.startVideo();
    QTest::qWait(300);
    const double fast = timeToLeave(200000);
    QVERIFY2(fast >= 0 && fast < 20, qPrintable(QStringLiteral("%1 against %2").arg(fast).arg(slow)));
}

void TestVoiceClient::aFrameArrivesAsPacketsThatReassemble()
{
    Call call(true);
    QTRY_VERIFY(call.isUp());
    call.startVideo();

    // More packets than leave in one go, so the order across pacing ticks is seen too.
    const EncodedVideoFrame frame = videoFrame(20000, true, 123456);
    constexpr int PACKETS = 18;
    call.client->sendVideoFrame(frame);
    QTRY_COMPARE(call.server.rtp.size(), PACKETS);
    QCOMPARE(call.server.undecryptable, 0);

    for (int i = 0; i < PACKETS; i++) {
        const RtpSeen &packet = call.server.rtp[i];
        QCOMPARE(packet.header.ssrc, OWN_VIDEO_SSRC);
        QCOMPARE(int(packet.header.payloadType), VIDEO_PAYLOAD_TYPE);
        QCOMPARE(packet.header.timestamp, 123456u);
        QCOMPARE(packet.header.sequence, static_cast<quint16>(call.server.rtp[0].header.sequence + i));
        QCOMPARE(packet.header.marker, i == PACKETS - 1);
        QVERIFY(packet.size <= VideoSendStream::MAX_PACKET_SIZE);

        // One numbering for everything this connection sends of video, beginning at one.
        const std::span<const uint8_t> transportSequence = findRtpExtension(packet.extensions, VideoSendStream::TRANSPORT_SEQUENCE_EXTENSION);
        QCOMPARE(transportSequence.size(), size_t(2));
        QCOMPARE(int(qFromBigEndian<quint16>(transportSequence.data())), i + 1);
        QCOMPARE(findRtpExtension(packet.extensions, VideoSendStream::ABS_SEND_TIME_EXTENSION).size(), size_t(3));
        QCOMPARE(findRtpExtension(packet.extensions, VideoSendStream::TRANSMISSION_OFFSET_EXTENSION).size(), size_t(3));
    }
    QCOMPARE(call.reassembled(VideoCodec::VP8, OWN_VIDEO_SSRC), frame.data);
}

void TestVoiceClient::stoppingVideoSaysSoAndSendsNothingMore()
{
    Call call(true);
    QTRY_VERIFY(call.isUp());
    call.startVideo();
    call.client->sendVideoFrame(videoFrame(1000, true, 0));
    QTRY_COMPARE(call.server.rtp.size(), 1);

    // What is still waiting to leave is dropped with the rest.
    call.client->sendVideoFrame(videoFrame(100000, false, 3000));
    call.client->stopVideo();
    QTRY_COMPARE(call.server.received(12).size(), 2);
    const QJsonObject off = call.server.received(12)[1];
    QCOMPARE(off["video_ssrc"].toInt(-1), 0);
    QCOMPARE(off["rtx_ssrc"].toInt(-1), 0);
    QCOMPARE(off["streams"].toArray()[0].toObject()["ssrc"].toInt(), int(OWN_VIDEO_SSRC));
    QCOMPARE(off["streams"].toArray()[0].toObject()["active"].toBool(true), false);

    call.client->sendVideoFrame(videoFrame(1000, true, 6000));
    call.server.sendFeedback({ nack(OWN_VIDEO_SSRC, call.server.rtp[0].header.sequence) });
    QTest::qWait(200);
    QCOMPARE(call.server.rtp.size(), 1);

    // Started again, it is announced again and carries on.
    call.startVideo();
    call.client->sendVideoFrame(videoFrame(1000, true, 9000));
    QTRY_COMPARE(call.server.rtp.size(), 2);
    QCOMPARE(call.server.received(12).size(), 3);
    QCOMPARE(call.server.received(12)[2]["video_ssrc"].toInt(), int(OWN_VIDEO_SSRC));
    QCOMPARE(call.server.rtp[1].header.sequence, static_cast<quint16>(call.server.rtp[0].header.sequence + 1));
}

void TestVoiceClient::aNackBringsThePacketBackOnTheRetransmissionStream()
{
    constexpr quint32 VIDEO_SSRC = 5000;
    constexpr quint32 RTX_SSRC = 5005;

    Call call(true);
    call.server.readyStreams = QJsonArray{ QJsonObject{ { "type", "video" }, { "rid", "100" }, { "ssrc", qint64(VIDEO_SSRC) }, { "rtx_ssrc", qint64(RTX_SSRC) }, { "quality", 100 }, { "active", false } } };
    QTRY_VERIFY(call.isUp());
    call.startVideo();

    call.client->sendVideoFrame(videoFrame(4000, true, 777));
    QTRY_COMPARE(call.server.rtp.size(), 4);
    const RtpSeen lost = call.server.rtp[2];
    QCOMPARE(lost.header.ssrc, VIDEO_SSRC);

    // Someone else's packet of the same number, and one this client never sent, bring nothing.
    call.server.sendFeedback({ nack(BOB_SSRC, lost.header.sequence), nack(VIDEO_SSRC, static_cast<quint16>(lost.header.sequence + 100)), nack(VIDEO_SSRC, lost.header.sequence) });
    QTRY_COMPARE(call.server.rtp.size(), 5);
    QTest::qWait(100);
    QCOMPARE(call.server.rtp.size(), 5);

    const RtpSeen &repeated = call.server.rtp[4];
    QCOMPARE(repeated.header.ssrc, RTX_SSRC);
    QCOMPARE(int(repeated.header.payloadType), RTX_PAYLOAD_TYPE);
    QCOMPARE(repeated.header.timestamp, 777u);
    QCOMPARE(repeated.payload.size(), lost.payload.size() + 2);
    QCOMPARE(qFromBigEndian<quint16>(repeated.payload.data()), lost.header.sequence);
    QCOMPARE(Bytes(repeated.payload.begin() + 2, repeated.payload.end()), lost.payload);
    QVERIFY(repeated.size <= VideoSendStream::MAX_PACKET_SIZE);

    const std::span<const uint8_t> transportSequence = findRtpExtension(repeated.extensions, VideoSendStream::TRANSPORT_SEQUENCE_EXTENSION);
    QCOMPARE(transportSequence.size(), size_t(2));
    QCOMPARE(int(qFromBigEndian<quint16>(transportSequence.data())), 5);
}

void TestVoiceClient::keyframeRequestsReachTheEncoderButNotInABurst()
{
    Call call(true);
    QTRY_VERIFY(call.isUp());
    call.startVideo();
    QSignalSpy keyframes(call.client.get(), &VoiceClient::videoKeyframeRequested);

    // About someone else's video.
    call.server.sendFeedback({ pictureLoss(BOB_SSRC), fullIntraRequest(BOB_SSRC) });
    QTest::qWait(100);
    QCOMPARE(keyframes.count(), 0);

    call.server.sendFeedback({ pictureLoss(OWN_VIDEO_SSRC) });
    QTRY_COMPARE(keyframes.count(), 1);

    // Every viewer asking at once is one more key frame, not one each.
    for (int i = 0; i < 5; i++)
        call.server.sendFeedback({ pictureLoss(OWN_VIDEO_SSRC), fullIntraRequest(OWN_VIDEO_SSRC) });
    QTest::qWait(100);
    QCOMPARE(keyframes.count(), 1);
    QTRY_COMPARE(keyframes.count(), 2);
    QTest::qWait(500);
    QCOMPARE(keyframes.count(), 2);

    // A full intra request by itself, long enough after, is passed on at once.
    call.server.sendFeedback({ fullIntraRequest(OWN_VIDEO_SSRC) });
    QTRY_COMPARE_WITH_TIMEOUT(keyframes.count(), 3, 200);
}

void TestVoiceClient::theReceiverCountIsPassedOn()
{
    Call call(true);
    QTRY_VERIFY(call.isUp());
    QSignalSpy receivers(call.client.get(), &VoiceClient::videoReceiversChanged);
    QSignalSpy keyframes(call.client.get(), &VoiceClient::videoKeyframeRequested);
    const auto counts = [&receivers] {
        QList<int> seen;
        for (const QList<QVariant> &arguments : receivers)
            seen.append(arguments[0].toInt());
        return seen;
    };

    // Known before video starts, when there is no encoder to ask anything of.
    call.server.sendFeedback({ receiverCount(BOB_SSRC, 4), receiverCount(OWN_VIDEO_SSRC, 0) });
    QTRY_COMPARE(counts(), QList<int>{ 0 });

    call.startVideo();
    call.server.sendFeedback({ receiverCount(OWN_VIDEO_SSRC, 2) });
    QTRY_COMPARE(counts(), (QList<int>{ 0, 2 }));
    // The first to watch has seen nothing yet.
    QTRY_COMPARE(keyframes.count(), 1);

    // Said again, one more of them, or none left: nothing for the encoder to do.
    call.server.sendFeedback({ receiverCount(OWN_VIDEO_SSRC, 2) });
    call.server.sendFeedback({ receiverCount(OWN_VIDEO_SSRC, 3) });
    call.server.sendFeedback({ receiverCount(OWN_VIDEO_SSRC, 0) });
    QTRY_COMPARE(counts(), (QList<int>{ 0, 2, 3, 0 }));
    QTest::qWait(400);
    QCOMPARE(keyframes.count(), 1);

    call.server.sendFeedback({ receiverCount(OWN_VIDEO_SSRC, 1) });
    QTRY_COMPARE(counts(), (QList<int>{ 0, 2, 3, 0, 1 }));
    QTRY_COMPARE(keyframes.count(), 2);
}

void TestVoiceClient::whatViewersWantIsPassedOn()
{
    Call call(true);
    QTRY_VERIFY(call.isUp());
    QSignalSpy wants(call.client.get(), &VoiceClient::videoWantsReceived);

    const QString own = QString::number(OWN_VIDEO_SSRC);
    const QString other = QString::number(BOB_SSRC);
    constexpr int MEDIA_SINK_WANTS = 15;
    call.server.send(MEDIA_SINK_WANTS, QJsonObject{ { "any", 100 } });
    call.server.send(MEDIA_SINK_WANTS, QJsonObject{ { own, 50 }, { "any", 100 }, { "pixelCounts", QJsonObject{ { own, 230400 }, { other, 1 } } } });
    call.server.send(MEDIA_SINK_WANTS, QJsonObject{ { other, 10 }, { "any", 20 }, { "pixelCounts", QJsonObject{ { other, 5 } } } });
    call.server.send(MEDIA_SINK_WANTS, QJsonObject{ { own, 0 }, { "any", 30 } });
    call.server.send(MEDIA_SINK_WANTS, QJsonObject{ { own, 0 }, { "any", 0 } });
    call.server.send(MEDIA_SINK_WANTS, QJsonObject{ { other, 10 } });
    QTRY_COMPARE(wants.count(), 6);

    const auto said = [&wants](int index) { return qMakePair(wants[index][0].toInt(), wants[index][1].toInt()); };
    QCOMPARE(said(0), qMakePair(100, 0));
    QCOMPARE(said(1), qMakePair(50, 230400));
    QCOMPARE(said(2), qMakePair(20, 0));
    QCOMPARE(said(3), qMakePair(30, 0));
    QCOMPARE(said(4), qMakePair(100, 0));
    QCOMPARE(said(5), qMakePair(100, 0));
}

void TestVoiceClient::aSenderReportAndStatisticsFollowEverySecond()
{
    Call call(true);
    QTRY_VERIFY(call.isUp());
    call.startVideo();
    QSignalSpy statistics(call.client.get(), &VoiceClient::videoSendStatsUpdated);

    call.client->sendVideoFrame(videoFrame(3000, true, 90000));
    QTRY_COMPARE(call.server.rtp.size(), 3);
    quint32 payloadBytes = 0;
    for (const RtpSeen &packet : call.server.rtp)
        payloadBytes += static_cast<quint32>(packet.payload.size());

    QTRY_VERIFY_WITH_TIMEOUT(!call.server.senderReports.isEmpty(), 1500);
    const ReceivedRtcp::SenderReport report = call.server.senderReports[0];
    QCOMPARE(report.ssrc, OWN_VIDEO_SSRC);
    QCOMPARE(report.packetCount, 3u);
    QCOMPARE(report.octetCount, payloadBytes);
    // The frame's own time moved on by however long ago it was sent: under a second and a half.
    QVERIFY(report.rtpTimestamp - 90000u < 135000u);
    QVERIFY(report.compactNtp != 0);
    QCOMPARE(call.server.undecryptable, 0);

    QTRY_VERIFY(statistics.count() >= 1);
    const VideoSendStats first = statistics[0][0].value<VideoSendStats>();
    QCOMPARE(first.codec, QStringLiteral("VP8"));
    QCOMPARE(first.packets, quint64(3));
    QCOMPARE(first.bytes, quint64(payloadBytes));
    QCOMPARE(first.frames, quint64(1));
    QCOMPARE(first.keyframes, quint64(1));
    // Three thousand bytes and their headers went out in about a second.
    QVERIFY(first.bitrateBps > 2000 * 8 && first.bitrateBps < 4000 * 8);
    QVERIFY(first.lossFraction < 0);
    QCOMPARE(first.roundTripMs, -1);
    QCOMPARE(first.receivers, -1);
    QCOMPARE(first.receiverEstimateBps, quint64(0));
    QCOMPARE(first.delayEstimateBps, quint64(0));
    QCOMPARE(first.acknowledgedBps, quint64(0));

    // The server answers the report, and says half of what was sent since did not arrive.
    ReceptionReport answer;
    answer.ssrc = OWN_VIDEO_SSRC;
    answer.fractionLost = 128;
    answer.lastSenderReport = report.compactNtp;
    // What it says of the audio in the same breath is not about the video.
    ReceptionReport aboutAudio;
    aboutAudio.ssrc = OWN_AUDIO_SSRC;
    aboutAudio.fractionLost = 255;
    const ReceptionReport answers[] = { answer, aboutAudio };
    call.server.sendFeedback({ receiverCount(OWN_VIDEO_SSRC, 2), pictureLoss(OWN_VIDEO_SSRC), receiverEstimate(2500000) }, answers);

    statistics.clear();
    QTRY_VERIFY_WITH_TIMEOUT(statistics.count() >= 1, 1500);
    const VideoSendStats second = statistics[0][0].value<VideoSendStats>();
    QCOMPARE(second.lossFraction, 0.5);
    QVERIFY(second.roundTripMs >= 0 && second.roundTripMs < 1500);
    QCOMPARE(second.receivers, 2);
    QCOMPARE(second.receiverEstimateBps, quint64(2500000));
    QCOMPARE(second.keyframeRequests, quint64(1));
    QCOMPARE(second.packets, quint64(3));
    QTRY_VERIFY(call.server.senderReports.size() >= 2);
}

void TestVoiceClient::aWayFoundToFallBehindIsSaidAtOnce()
{
    Call call(true);
    QTRY_VERIFY(call.isUp());
    call.startVideo();
    call.client->setVideoBitrate(1'000'000);
    QSignalSpy statistics(call.client.get(), &VoiceClient::videoSendStatsUpdated);

    // About a megabit for a second and a half: four kilobytes thirty times a second.
    constexpr int FRAMES = 45;
    for (int i = 0; i < FRAMES; i++) {
        call.client->sendVideoFrame(videoFrame(4000, i == 0, 90000 + static_cast<quint32>(i) * 3000));
        QTest::qWait(33);
    }
    QTRY_VERIFY(call.server.rtp.size() >= FRAMES * 4);

    // The server's reports on them, as of a way that carries half of what was sent: no packet
    // is through before the one ahead of it. Half of what was sent and not half a megabit,
    // since a busy machine sends these more slowly, down to where half a megabit carries it all.
    double bitsSent = 0;
    for (const RtpSeen &packet : std::as_const(call.server.rtp))
        bitsSent += packet.size * 8;
    const double sentFor = sendTimeOf(call.server.rtp.last()) - sendTimeOf(call.server.rtp.first());
    QVERIFY(sentFor > 0);
    const double carried = bitsSent / sentFor / 2;
    TransportFeedback server;
    double throughAt = 0;
    for (const RtpSeen &packet : std::as_const(call.server.rtp)) {
        const std::span<const uint8_t> number = findRtpExtension(packet.extensions, VideoSendStream::TRANSPORT_SEQUENCE_EXTENSION);
        QCOMPARE(number.size(), size_t(2));
        throughAt = std::max(throughAt, sendTimeOf(packet)) + packet.size * 8 / carried;
        server.onPacket(qFromBigEndian<quint16>(number.data()), static_cast<qint64>((throughAt + 100) * 1e6));
    }

    // Straight after the statistics, so that the next of those is a second off.
    statistics.clear();
    QTRY_VERIFY_WITH_TIMEOUT(statistics.count() >= 1, 1500);
    QCOMPARE(statistics.last()[0].value<VideoSendStats>().delayEstimateBps, quint64(0));
    statistics.clear();
    while (server.hasPending()) {
        Bytes report(1200);
        const int size = server.write(report, SERVER_SSRC, OWN_VIDEO_SSRC);
        QVERIFY(size > 0);
        report.resize(static_cast<size_t>(size));
        call.server.sendFeedback({ report });
    }

    QTRY_VERIFY2_WITH_TIMEOUT(statistics.count() >= 1, qPrintable(QStringLiteral("nothing said of a way that carries %1 bps").arg(carried)), 500);
    const VideoSendStats said = statistics.last()[0].value<VideoSendStats>();
    QVERIFY2(said.acknowledgedBps > carried * 0.86 && said.acknowledgedBps < carried * 1.14, qPrintable(QStringLiteral("%1 of %2").arg(said.acknowledgedBps).arg(carried)));
    // A little under what gets through.
    QVERIFY2(said.delayEstimateBps > carried * 0.66 && said.delayEstimateBps < said.acknowledgedBps, qPrintable(QStringLiteral("%1 of %2").arg(said.delayEstimateBps).arg(carried)));
}

void TestVoiceClient::aChangeOfCodecIsTakenUp()
{
    Call call(true);
    QTRY_VERIFY(call.isUp());
    call.startVideo();
    QSignalSpy codecs(call.client.get(), &VoiceClient::videoSendCodecChanged);
    constexpr int SESSION_UPDATE = 14;

    // Something else about the session changes: the codec stays.
    call.server.send(SESSION_UPDATE, QJsonObject{ { "media_session_id", "abc" } });
    call.server.send(SESSION_UPDATE, QJsonObject{ { "video_codec", "H264" } });
    QTRY_COMPARE(codecs.count(), 1);
    QCOMPARE(codecs[0][0].value<VideoCodec>(), VideoCodec::H264);
    QCOMPARE(call.client->videoSendCodec(), VideoCodec::H264);

    // The new codec's stream opens with a key frame. Anything before one is left out, even
    // when it would packetize: it may be a frame the old encoder still had in hand.
    EncodedVideoFrame delta;
    delta.data = { 0, 0, 0, 1, 0x41, 0x88, 0x5A, 0x5A };
    delta.rtpTimestamp = 3000;
    call.client->sendVideoFrame(delta);
    const EncodedVideoFrame keyframe = h264Keyframe(6000);
    call.client->sendVideoFrame(keyframe);
    QTRY_COMPARE(call.server.rtp.size(), 4);
    for (const RtpSeen &packet : call.server.rtp) {
        QCOMPARE(int(packet.header.payloadType), H264_PAYLOAD_TYPE);
        QCOMPARE(packet.header.timestamp, 6000u);
    }
    // The parameter sets travel together, ahead of the picture's fragments.
    QCOMPARE(call.server.rtp[0].payload[0] & 0x1F, 24);
    QCOMPARE(call.server.rtp[1].payload[0] & 0x1F, 28);
    QCOMPARE(call.reassembled(VideoCodec::H264, OWN_VIDEO_SSRC), keyframe.data);

    call.server.sendFeedback({ nack(OWN_VIDEO_SSRC, call.server.rtp[1].header.sequence) });
    QTRY_COMPARE(call.server.rtp.size(), 5);
    QCOMPARE(call.server.rtp[4].header.ssrc, OWN_RTX_SSRC);
    QCOMPARE(int(call.server.rtp[4].header.payloadType), H264_RTX_PAYLOAD_TYPE);

    // A codec this client never offered to encode: there is nothing to send video with.
    call.server.send(SESSION_UPDATE, QJsonObject{ { "video_codec", "VP9" } });
    QTRY_COMPARE(codecs.count(), 2);
    QCOMPARE(codecs[1][0].value<VideoCodec>(), VideoCodec::Unknown);
    call.client->sendVideoFrame(keyframe);
    QTest::qWait(150);
    QCOMPARE(call.server.rtp.size(), 5);
}

void TestVoiceClient::aChangeToAv1WaitsForItsKeyframe()
{
    constexpr int SESSION_UPDATE = 14;

    Call call(true);
    call.server.videoCodec = QStringLiteral("H264");
    QTRY_VERIFY(call.isUp());
    QCOMPARE(call.client->videoSendCodec(), VideoCodec::H264);
    call.startVideo();
    QSignalSpy codecs(call.client.get(), &VoiceClient::videoSendCodecChanged);

    const EncodedVideoFrame h264 = h264Keyframe(3000);
    call.client->sendVideoFrame(h264);
    QTRY_COMPARE(call.server.rtp.size(), 4);
    QCOMPARE(int(call.server.rtp[0].header.payloadType), H264_PAYLOAD_TYPE);
    QCOMPARE(call.reassembled(VideoCodec::H264, OWN_VIDEO_SSRC), h264.data);
    const quint16 lastOfH264 = call.server.rtp[3].header.sequence;
    call.server.rtp.clear();

    call.server.send(SESSION_UPDATE, QJsonObject{ { "video_codec", "AV1" } });
    QTRY_COMPARE(codecs.count(), 1);
    QCOMPARE(call.client->videoSendCodec(), VideoCodec::AV1);

    // A frame that builds on pictures nobody was sent in this codec stays here.
    call.client->sendVideoFrame(av1Frame(false, 6000));
    QTest::qWait(150);
    QVERIFY(call.server.rtp.isEmpty());

    call.client->sendVideoFrame(av1Frame(true, 9000));
    call.client->sendVideoFrame(av1Frame(false, 12000));
    QTRY_COMPARE(call.server.rtp.size(), 6);
    for (int i = 0; i < 6; i++) {
        const RtpSeen &packet = call.server.rtp[i];
        QCOMPARE(int(packet.header.payloadType), AV1_PAYLOAD_TYPE);
        QCOMPARE(packet.header.timestamp, i < 3 ? 9000u : 12000u);
        QCOMPARE(packet.header.sequence, static_cast<quint16>(lastOfH264 + 1 + i));
        QCOMPARE(packet.header.marker, i == 2 || i == 5);
    }
    QVERIFY(startsKeyframe(VideoCodec::AV1, call.server.rtp[0].payload));
    QVERIFY(!startsKeyframe(VideoCodec::AV1, call.server.rtp[3].payload));

    const QList<RtpSeen> both = call.server.rtp;
    call.server.rtp = both.mid(0, 3);
    QCOMPARE(call.reassembled(VideoCodec::AV1, OWN_VIDEO_SSRC), av1AsAssembled(true));
    call.server.rtp = both.mid(3);
    QCOMPARE(call.reassembled(VideoCodec::AV1, OWN_VIDEO_SSRC), av1AsAssembled(false));
    QCOMPARE(call.server.undecryptable, 0);
}

void TestVoiceClient::encodersFoundLateAreOfferedInASessionUpdate()
{
    constexpr int SESSION_UPDATE = 14;

    // Connected while it was not known what this machine encodes: VP8 is offered for decoding
    // alone, and the codec the server names cannot be taken up.
    Call call;
    QTRY_VERIFY(call.isUp());
    QCOMPARE(call.server.received(1)[0]["codecs"].toArray()[1].toObject()["encode"].toBool(), false);
    QCOMPARE(call.client->videoSendCodec(), VideoCodec::Unknown);
    QSignalSpy codecs(call.client.get(), &VoiceClient::videoSendCodecChanged);

    // H264 was never offered on this connection, so it is not offered now either.
    call.client->offerVideoEncoding({ VideoCodec::H264, VideoCodec::VP8 });
    QTRY_COMPARE(call.server.receivedText(SESSION_UPDATE).size(), 1);
    QCOMPARE(call.server.receivedText(SESSION_UPDATE)[0],
             QByteArray(R"({"op":14,"d":{"codecs":[{"name":"opus","type":"audio","priority":1000,"payload_type":120},{"name":"VP8","type":"video","priority":1000,"payload_type":101,"rtx_payload_type":102,"encode":true,"decode":true}]}})"));

    // The codec the server had named at the start is the one to send with after all.
    QCOMPARE(codecs.count(), 1);
    QCOMPARE(call.client->videoSendCodec(), VideoCodec::VP8);

    // Nothing more to offer, nothing more said.
    call.client->offerVideoEncoding({ VideoCodec::VP8 });
    QTest::qWait(100);
    QCOMPARE(call.server.receivedText(SESSION_UPDATE).size(), 1);

    // And video goes out on the connection as it is.
    call.startVideo();
    QTRY_COMPARE(call.server.receivedText(12).size(), 1);
    call.client->sendVideoFrame(videoFrame(3000, true, 0));
    QTRY_VERIFY(!call.server.rtp.isEmpty());
    QCOMPARE(int(call.server.rtp[0].header.payloadType), VIDEO_PAYLOAD_TYPE);
    QCOMPARE(call.server.rtp[0].header.ssrc, OWN_VIDEO_SSRC);
}

void TestVoiceClient::encodersFoundBeforeTheProtocolIsChosenNeedNoUpdate()
{
    Call call;
    call.client->offerVideoEncoding({ VideoCodec::VP8 });
    QTRY_VERIFY(call.isUp());

    QCOMPARE(call.server.received(1)[0]["codecs"].toArray()[1].toObject()["encode"].toBool(), true);
    QCOMPARE(call.client->videoSendCodec(), VideoCodec::VP8);
    QTest::qWait(100);
    QVERIFY(call.server.received(14).isEmpty());
}

void TestVoiceClient::aCodecWithdrawnWhileInUseSendsNothingUntilAnotherIsNamed()
{
    constexpr int SESSION_UPDATE = 14;

    Call call(true);
    call.server.videoCodec = QStringLiteral("AV1");
    QTRY_VERIFY(call.isUp());
    QCOMPARE(call.client->videoSendCodec(), VideoCodec::AV1);
    call.startVideo();
    QTRY_COMPARE(call.server.receivedText(12).size(), 1);
    call.client->sendVideoFrame(av1Frame(true, 3000));
    QTRY_COMPARE(call.server.rtp.size(), 3);
    QCOMPARE(int(call.server.rtp[0].header.payloadType), AV1_PAYLOAD_TYPE);
    call.server.rtp.clear();
    QSignalSpy codecs(call.client.get(), &VoiceClient::videoSendCodecChanged);

    // The server hears of the codecs that are left. AV1 was offered for sending alone, and a
    // codec that is neither sent nor received is not offered at all.
    call.client->offerVideoEncoding({ VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 });
    QTRY_COMPARE(call.server.receivedText(SESSION_UPDATE).size(), 1);
    const QJsonArray offered = call.server.received(SESSION_UPDATE)[0]["codecs"].toArray();
    QStringList names;
    for (const auto &each : offered) {
        names.append(each.toObject()["name"].toString());
        if (each.toObject()["type"].toString() == QStringLiteral("video"))
            QCOMPARE(each.toObject()["encode"].toBool(), true);
    }
    QCOMPARE(names, QStringList({ QStringLiteral("opus"), QStringLiteral("VP8"), QStringLiteral("H264"), QStringLiteral("H265") }));

    // Until the server answers there is nothing to send with, and nothing is sent.
    QCOMPARE(codecs.count(), 1);
    QCOMPARE(codecs[0][0].value<VideoCodec>(), VideoCodec::Unknown);
    QCOMPARE(call.client->videoSendCodec(), VideoCodec::Unknown);
    call.client->sendVideoFrame(av1Frame(true, 6000));
    QTest::qWait(150);
    QVERIFY(call.server.rtp.isEmpty());

    // A server that names the withdrawn codec again is not followed either.
    call.server.send(SESSION_UPDATE, QJsonObject{ { "video_codec", "AV1" } });
    QTest::qWait(150);
    QCOMPARE(codecs.count(), 1);
    QCOMPARE(call.client->videoSendCodec(), VideoCodec::Unknown);

    call.server.send(SESSION_UPDATE, QJsonObject{ { "video_codec", "H265" } });
    QTRY_COMPARE(codecs.count(), 2);
    QCOMPARE(call.client->videoSendCodec(), VideoCodec::H265);
    const EncodedVideoFrame h265 = h265Keyframe(9000);
    call.client->sendVideoFrame(h265);
    QTRY_VERIFY(!call.server.rtp.isEmpty());
    QCOMPARE(int(call.server.rtp[0].header.payloadType), H265_PAYLOAD_TYPE);
    QCOMPARE(call.server.rtp[0].header.timestamp, 9000u);
    QTRY_COMPARE(call.reassembled(VideoCodec::H265, OWN_VIDEO_SSRC), h265.data);
    QCOMPARE(call.server.undecryptable, 0);
}

void TestVoiceClient::onlyACommitThatGivesSomeoneAKeyWantsAKeyframe()
{
    using discord::dave::RosterMap;
    const std::vector<uint8_t> key = { 1, 2, 3 };

    QVERIFY(!bringsNewKeys(RosterMap{}));
    // Someone joined, or someone's key changed.
    QVERIFY(bringsNewKeys(RosterMap{ { 100, key } }));
    // Someone left: they are in the changes without a key, and everyone still here has seen
    // what came before.
    QVERIFY(!bringsNewKeys(RosterMap{ { 100, {} } }));
    QVERIFY(!bringsNewKeys(RosterMap{ { 100, {} }, { 200, {} } }));
    QVERIFY(bringsNewKeys(RosterMap{ { 100, {} }, { 200, key } }));
}

void TestVoiceClient::noVideoGoesOutWhileEndToEndKeysAreMissing()
{
    Call call(true);
    call.server.daveProtocolVersion = 1;
    QTRY_VERIFY(call.isUp());
    QSignalSpy keyframes(call.client.get(), &VoiceClient::videoKeyframeRequested);

    // Announced all the same; only the pictures wait.
    call.startVideo();
    call.client->sendVideoFrame(videoFrame(3000, true, 0));
    call.client->sendVideoFrame(videoFrame(3000, false, 3000));
    QTRY_COMPARE(call.server.received(12).size(), 1);
    QTest::qWait(200);
    QVERIFY(!call.client->isDaveEnabled());
    QVERIFY(call.server.rtp.isEmpty());
    QCOMPARE(keyframes.count(), 0);

    // The call goes on without end-to-end encryption, so the frames may leave as they are.
    constexpr int DAVE_PREPARE_TRANSITION = 21;
    constexpr int DAVE_EXECUTE_TRANSITION = 22;
    call.server.send(DAVE_PREPARE_TRANSITION, QJsonObject{ { "protocol_version", 0 }, { "transition_id", 5 } });
    call.server.send(DAVE_EXECUTE_TRANSITION, QJsonObject{ { "transition_id", 5 } });
    QTRY_COMPARE(keyframes.count(), 1);

    // Sending picks up at a key frame; what builds on frames nobody was sent is left out.
    call.client->sendVideoFrame(videoFrame(3000, false, 6000));
    const EncodedVideoFrame keyframe = videoFrame(3000, true, 9000);
    call.client->sendVideoFrame(keyframe);
    call.client->sendVideoFrame(videoFrame(100, false, 12000));
    QTRY_COMPARE(call.server.rtp.size(), 4);
    for (int i = 0; i < 3; i++)
        QCOMPARE(call.server.rtp[i].header.timestamp, 9000u);
    QCOMPARE(call.server.rtp[3].header.timestamp, 12000u);
    call.server.rtp.removeLast();
    QCOMPARE(call.reassembled(VideoCodec::VP8, OWN_VIDEO_SSRC), keyframe.data);
}

void TestVoiceClient::sharedSoundIsAnnouncedOnceAndMarkedAtEveryRun()
{
    constexpr int OPUS_PAYLOAD_TYPE = 120;
    constexpr quint32 SAMPLES_PER_FRAME = 960;
    constexpr int RUNS = 3;
    constexpr int FRAMES_PER_RUN = 4;
    constexpr int BETWEEN_RUNS_MS = 150;

    Call call(true, Call::Kind::StreamWithSound);
    QTRY_VERIFY(call.isUp());

    // Nothing is shared yet: no word of it, and not the silence a voice connection opens with.
    QTest::qWait(200);
    QVERIFY(call.server.rtp.isEmpty());
    QVERIFY(call.server.received(5).isEmpty());

    for (int run = 0; run < RUNS; run++) {
        call.client->beginSoundshare();
        for (int frame = 0; frame < FRAMES_PER_RUN; frame++)
            call.client->sendAudio(QByteArray(40 + frame, static_cast<char>(run * 16 + frame)));
        QTRY_COMPARE(call.server.rtp.size(), (run + 1) * FRAMES_PER_RUN);
        QTest::qWait(BETWEEN_RUNS_MS);
    }
    QCOMPARE(call.server.undecryptable, 0);

    for (int i = 0; i < RUNS * FRAMES_PER_RUN; i++) {
        const RtpSeen &packet = call.server.rtp[i];
        const int frame = i % FRAMES_PER_RUN;
        QCOMPARE(packet.header.ssrc, OWN_AUDIO_SSRC);
        QCOMPARE(int(packet.header.payloadType), OPUS_PAYLOAD_TYPE);
        QCOMPARE(packet.header.marker, frame == 0);
        QCOMPARE(packet.header.sequence, static_cast<quint16>(call.server.rtp[0].header.sequence + i));
        QVERIFY(packet.extensions.empty());
        QCOMPARE(packet.payload, Bytes(static_cast<size_t>(40 + frame), static_cast<uint8_t>(i / FRAMES_PER_RUN * 16 + frame)));

        if (i == 0)
            continue;
        // Within a run the frames follow one another. A new run is timed by the clock again,
        // so the pause before it shows.
        const quint32 sincePrevious = packet.header.timestamp - call.server.rtp[i - 1].header.timestamp;
        if (frame != 0)
            QCOMPARE(sincePrevious, SAMPLES_PER_FRAME);
        else
            QVERIFY2(sincePrevious > SAMPLES_PER_FRAME * 2, qPrintable(QString::number(sincePrevious)));
    }

    QCOMPARE(call.server.receivedText(5), QList<QByteArray>{ R"({"op":5,"d":{"speaking":2,"delay":0,"ssrc":777}})" });
}

void TestVoiceClient::aStreamWithoutSoundSendsNoAudio()
{
    Call call(true, Call::Kind::Stream);
    QTRY_VERIFY(call.isUp());

    call.client->beginSoundshare();
    call.client->sendAudio(QByteArray(40, 'a'));
    QTest::qWait(200);

    QVERIFY(call.server.rtp.isEmpty());
    QVERIFY(call.server.received(5).isEmpty());
    QCOMPARE(call.server.undecryptable, 0);
}

void TestVoiceClient::aProxiedConnectionReachesTheServerThroughTheProxyAlone_data()
{
    QTest::addColumn<bool>("streamWithSound");
    QTest::newRow("camera on a voice connection") << false;
    QTest::newRow("screen share with its sound") << true;
}

void TestVoiceClient::aProxiedConnectionReachesTheServerThroughTheProxyAlone()
{
    QFETCH(bool, streamWithSound);
    const Call::Kind kind = streamWithSound ? Call::Kind::StreamWithSound : Call::Kind::Voice;

    FakeSocks5Proxy proxy;
    Call call(true, kind, proxy.config());
    QTRY_VERIFY(call.isUp());

    QCOMPARE(proxy.connectedTo, QList<quint16>{ call.server.gatewayPort() });
    QCOMPARE(call.server.gatewayPeerPorts, proxy.onwardPorts);
    QVERIFY(proxy.associated);

    call.receiveAliceAndBob();
    call.server.sendFrame(ALICE_SSRC, 0, 100, true);
    QTRY_COMPARE(call.framesFrom(ALICE), 1);

    call.startVideo();
    call.client->sendVideoFrame(videoFrame(20000, true, 123456));
    if (streamWithSound) {
        call.client->beginSoundshare();
        call.client->sendAudio(QByteArray(40, 'a'));
    }
    QTRY_VERIFY(call.server.rtp.size() >= 18);
    QTRY_VERIFY(call.server.acknowledgedUpTo > 0);

    QCOMPARE(call.server.mediaSenderPorts, QSet<quint16>{ proxy.relayPort() });
    QVERIFY(proxy.datagramsToServer > 0);
    QVERIFY(proxy.datagramsToClient > 0);
    QCOMPARE(call.server.undecryptable, 0);
}

namespace {

Core::StreamRecord readyAt(const FakeVoiceServer &server, const QString &token = QStringLiteral("token"))
{
    Core::StreamRecord record;
    record.state = Core::StreamRecord::State::Ready;
    record.rtcServerId = Core::Snowflake(500);
    record.endpoint = server.endpoint();
    record.token = token;
    return record;
}

std::unique_ptr<Core::StreamConnection> streamConnection(const Core::ProxyConfig &proxy = {})
{
    const Core::StreamConnection::Account account{ Core::Snowflake(3), QStringLiteral("session"), proxy };
    return std::make_unique<Core::StreamConnection>(account, [](ConnectionParams &params) { params.gatewayScheme = QStringLiteral("ws"); });
}

Core::StreamConnection::Failure failureOf(const QSignalSpy &failed)
{
    return failed.first().first().value<Core::StreamConnection::Failure>();
}

} // namespace

void TestVoiceClient::aStreamConnectionIsMadeWhereTheGatewaySays()
{
    FakeVoiceServer server;
    const auto rtc = streamConnection();
    QSignalSpy opened(rtc.get(), &Core::StreamConnection::opened);
    QSignalSpy connected(rtc.get(), &Core::StreamConnection::connected);

    rtc->apply(readyAt(server));
    QTRY_COMPARE(connected.size(), 1);
    QCOMPARE(opened.size(), 1);
    QVERIFY(rtc->isConnected());

    // Named by the gateway's RTC server, in the channel one below it.
    const QList<QJsonObject> identify = server.received(0);
    QCOMPARE(identify.size(), 1);
    QCOMPARE(identify.first()["server_id"].toString(), QStringLiteral("500"));
    QCOMPARE(identify.first()["channel_id"].toString(), QStringLiteral("499"));
    QCOMPARE(identify.first()["streams"].toArray().first().toObject()["type"].toString(), QStringLiteral("screen"));

    // The same place again changes nothing.
    rtc->apply(readyAt(server));
    QTest::qWait(200);
    QCOMPARE(opened.size(), 1);
    QCOMPARE(server.gatewayConnections(), 1);
}

void TestVoiceClient::aDroppedStreamConnectionIsMadeAgainAfterAPing()
{
    FakeVoiceServer server;
    const auto rtc = streamConnection();
    QSignalSpy connected(rtc.get(), &Core::StreamConnection::connected);
    QSignalSpy reconnecting(rtc.get(), &Core::StreamConnection::reconnecting);
    QSignalSpy pings(rtc.get(), &Core::StreamConnection::pingRequested);
    QSignalSpy closed(rtc.get(), &Core::StreamConnection::closed);

    rtc->apply(readyAt(server));
    QTRY_COMPARE(connected.size(), 1);

    server.closeWith(4014);
    QTRY_COMPARE(reconnecting.size(), 1);
    QCOMPARE(pings.size(), 1);
    QCOMPARE(closed.size(), 1);
    QVERIFY(!rtc->isConnected());

    // The gateway answers the ping with the same place before the delay is over. That makes
    // the one connection, and the delay running out after it does not make another.
    rtc->apply(readyAt(server));
    QTRY_COMPARE(connected.size(), 2);
    QTest::qWait(Core::StreamConnection::RECONNECT_DELAY_MS + 300);
    QCOMPARE(server.gatewayConnections(), 2);
    QCOMPARE(connected.size(), 2);
    QVERIFY(rtc->isConnected());
}

void TestVoiceClient::aStreamConnectionDroppingEveryTimeIsGivenUpOn()
{
    FakeVoiceServer server;
    const auto rtc = streamConnection();
    QSignalSpy opened(rtc.get(), &Core::StreamConnection::opened);
    QSignalSpy failed(rtc.get(), &Core::StreamConnection::failed);
    QSignalSpy pings(rtc.get(), &Core::StreamConnection::pingRequested);

    // Every connection drops before it is up, and the gateway names the same place again
    // whenever it is pinged, as it does: that does not make the count of drops start over.
    server.dropOnIdentify = true;
    QObject::connect(rtc.get(), &Core::StreamConnection::pingRequested, rtc.get(), [&rtc, &server] { rtc->apply(readyAt(server)); });

    rtc->apply(readyAt(server));
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 20000);
    QCOMPARE(failureOf(failed), Core::StreamConnection::Failure::Lost);
    QCOMPARE(pings.size(), Core::StreamConnection::MAX_RECONNECTS);
    QCOMPARE(opened.size(), Core::StreamConnection::MAX_RECONNECTS + 1);
    QVERIFY(!rtc->connection());
}

void TestVoiceClient::aStreamThatMovesIsFollowed()
{
    FakeVoiceServer first;
    FakeVoiceServer second;
    const auto rtc = streamConnection();
    QSignalSpy connected(rtc.get(), &Core::StreamConnection::connected);
    QSignalSpy reconnecting(rtc.get(), &Core::StreamConnection::reconnecting);

    rtc->apply(readyAt(first));
    QTRY_COMPARE(connected.size(), 1);

    Core::StreamRecord moving;
    moving.state = Core::StreamRecord::State::Reconnecting;
    rtc->apply(moving);
    QCOMPARE(reconnecting.size(), 1);
    QVERIFY(!rtc->connection());

    rtc->apply(readyAt(second, QStringLiteral("another token")));
    QTRY_COMPARE(connected.size(), 2);
    QCOMPARE(second.received(0).first()["token"].toString(), QStringLiteral("another token"));
}

void TestVoiceClient::aStreamConnectionNotUpInTimeFails()
{
    // Takes the connection and never answers it.
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress(QHostAddress::LocalHost)));
    const auto rtc = streamConnection();
    QSignalSpy failed(rtc.get(), &Core::StreamConnection::failed);

    Core::StreamRecord record;
    record.state = Core::StreamRecord::State::Ready;
    record.rtcServerId = Core::Snowflake(500);
    record.endpoint = QStringLiteral("127.0.0.1:%1").arg(silent.serverPort());
    record.token = QStringLiteral("token");
    rtc->apply(record);

    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, Core::StreamConnection::CONNECT_TIMEOUT_MS + 3000);
    QCOMPARE(failureOf(failed), Core::StreamConnection::Failure::TimedOut);
    QVERIFY(!rtc->connection());
}

void TestVoiceClient::aStreamConnectionRefusesAProxyThatCannotCarryIt()
{
    FakeVoiceServer server;
    Core::ProxyConfig http;
    http.type = Core::ProxyConfig::Type::Http;
    http.host = QStringLiteral("127.0.0.1");
    http.port = 8080;
    const auto rtc = streamConnection(http);
    QSignalSpy failed(rtc.get(), &Core::StreamConnection::failed);

    rtc->apply(readyAt(server));
    QCOMPARE(failed.size(), 1);
    QCOMPARE(failureOf(failed), Core::StreamConnection::Failure::ProxyRefused);
    QTest::qWait(200);
    QCOMPARE(server.gatewayConnections(), 0);
}

QTEST_GUILESS_MAIN(TestVoiceClient)
#include "tst_VoiceClient.moc"
