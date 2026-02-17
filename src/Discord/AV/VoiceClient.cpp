#include "VoiceClient.hpp"
#include "VoiceGateway.hpp"
#include "UdpTransport.hpp"
#include "VoiceEncryption.hpp"
#include "RtpPacket.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Discord {
namespace AV {

static constexpr uint8_t OPUS_SILENCE[] = { 0xF8, 0xFF, 0xFE };

// 20ms at 48khz
static constexpr uint32_t OPUS_FRAME_SAMPLES = 960;

VoiceClient::VoiceClient(const QString &endpoint, const QString &token,
                         Core::Snowflake serverId, Core::Snowflake channelId,
                         Core::Snowflake userId, const QString &sessionId,
                         QObject *parent)
    : QObject(parent),
      endpoint(endpoint),
      token(token),
      serverId(serverId),
      channelId(channelId),
      userId(userId),
      sessionId(sessionId)
{
}

VoiceClient::~VoiceClient()
{
    stop();
}

void VoiceClient::start()
{
    if (currentState != State::Disconnected) {
        qCWarning(LogVoice) << "VoiceClient::start called in non-disconnected state";
        return;
    }

    VoiceEncryption::initialize();

    setState(State::Connecting);

    gateway = new VoiceGateway(endpoint, serverId, channelId, userId, sessionId, token, this);

    connect(gateway, &VoiceGateway::connected, this, &VoiceClient::onGatewayConnected);
    connect(gateway, &VoiceGateway::disconnected, this, &VoiceClient::onGatewayDisconnected);
    connect(gateway, &VoiceGateway::readyReceived, this, &VoiceClient::onGatewayReady);
    connect(gateway, &VoiceGateway::sessionDescriptionReceived, this, &VoiceClient::onSessionDescription);
    connect(gateway, &VoiceGateway::speakingReceived, this, &VoiceClient::onSpeaking);
    connect(gateway, &VoiceGateway::clientConnected, this, &VoiceClient::onClientConnect);
    connect(gateway, &VoiceGateway::clientDisconnected, this, &VoiceClient::onClientDisconnect);
    connect(gateway, &VoiceGateway::resumed, this, &VoiceClient::onGatewayResumed);

    gateway->start();
}

void VoiceClient::stop()
{
    if (currentState == State::Disconnected)
        return;

    if (gateway) {
        gateway->hardStop();
        delete gateway;
        gateway = nullptr;
    }

    cleanupTransport();

    localSsrc = 0;
    selectedMode.clear();
    sessionKey.clear();
    ssrcToUser.clear();

    setState(State::Disconnected);
    // No emit disconnected() here — stop() is synchronous teardown,
    // callers handle their own cleanup. The disconnected() signal is only
    // emitted from onGatewayDisconnected for unexpected disconnections.
}

void VoiceClient::cleanupTransport()
{
    if (keepaliveTimer) {
        keepaliveTimer->stop();
        delete keepaliveTimer;
        keepaliveTimer = nullptr;
    }

    encryption.reset();

    delete udpTransport;
    udpTransport = nullptr;

    rtpSequence = 0;
    rtpTimestamp = 0;
}

void VoiceClient::onGatewayConnected()
{
    qCInfo(LogVoice) << "Voice gateway WebSocket connected, waiting for Hello + Identify";
    setState(State::Identifying);
}

void VoiceClient::onGatewayDisconnected(VoiceCloseCode code, const QString &reason)
{
    qCWarning(LogVoice) << "Voice gateway disconnected, code:" << code << "reason:" << reason;

    cleanupTransport();

    // done if not reconnected
    if (currentState != State::Disconnected) {
        setState(State::Disconnected);
        emit disconnected();
    }
}

void VoiceClient::onGatewayReady(const VoiceReady &data)
{
    qCInfo(LogVoice) << "Voice Ready: SSRC =" << data.ssrc
                     << "server =" << data.ip << ":" << data.port
                     << "modes =" << data.modes.get();

    localSsrc = data.ssrc;
    serverIp = data.ip;
    serverPort = data.port;
    serverModes = data.modes;

    static const std::array preferred = {
        EncryptionMode::AEAD_AES256_GCM_RTPSIZE,
        EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE,
    };

    EncryptionMode mode = EncryptionMode::UNKNOWN;
    for (auto candidate : preferred) {
        if (serverModes.contains(encryptionModeToString(candidate)) && VoiceEncryption::isModeAvailable(candidate)) {
            mode = candidate;
            break;
        }
    }

    if (mode == EncryptionMode::UNKNOWN) {
        qCCritical(LogVoice) << "No supported encryption mode found! Server offered:" << serverModes;
        stop();
        return;
    }
    selectedMode = encryptionModeToString(mode);
    qCInfo(LogVoice) << "Selected encryption mode:" << selectedMode;

    setState(State::DiscoveringIP);

    cleanupTransport();

    udpTransport = new UdpTransport(this);
    connect(udpTransport, &UdpTransport::ipDiscovered, this, &VoiceClient::onIpDiscovered);
    connect(udpTransport, &UdpTransport::ipDiscoveryFailed, this, &VoiceClient::onIpDiscoveryFailed);
    connect(udpTransport, &UdpTransport::datagramReceived, this, &VoiceClient::onDatagram);

    udpTransport->startIpDiscovery(serverIp, serverPort, localSsrc);
}

void VoiceClient::onSessionDescription(const SessionDescription &desc)
{
    qCInfo(LogVoice) << "Session established: mode =" << desc.mode
                     << "key length =" << desc.secretKey->size();

    sessionKey = desc.secretKey;
    selectedMode = desc.mode;

    EncryptionMode mode = encryptionModeFromString(selectedMode);
    encryption = std::make_unique<VoiceEncryption>(mode, sessionKey);

    setState(State::Connected);
    emit connected();

    // send silence so discord sends us audio immediately
    sendSilence();

    if (!keepaliveTimer) {
        keepaliveTimer = new QTimer(this);
        connect(keepaliveTimer, &QTimer::timeout, this, &VoiceClient::sendSilence);
    }
    keepaliveTimer->start(KEEPALIVE_INTERVAL_MS);
}

void VoiceClient::sendSilence()
{
    if (currentState != State::Connected || !encryption || !udpTransport)
        return;

    RtpHeader header;
    header.payloadType = 120;
    header.sequence = rtpSequence++;
    header.timestamp = rtpTimestamp;
    header.ssrc = localSsrc;

    rtpTimestamp += OPUS_FRAME_SAMPLES;

    QByteArray headerBytes = header.serialize();
    QByteArray silencePayload(reinterpret_cast<const char *>(OPUS_SILENCE), sizeof(OPUS_SILENCE));

    QByteArray encryptedSection = encryption->encrypt(headerBytes, silencePayload);
    if (encryptedSection.isEmpty()) {
        qCWarning(LogVoice) << "Failed to encrypt silence frame";
        return;
    }

    QByteArray packet = headerBytes + encryptedSection;
    udpTransport->send(packet);

    qCDebug(LogVoice) << "Sent silence frame, seq =" << header.sequence
                      << "ts =" << header.timestamp
                      << "packet size =" << packet.size();
}

void VoiceClient::onDatagram(const QByteArray &data)
{
    if (data.size() < RtpHeader::FIXED_SIZE)
        return;

    const auto *p = reinterpret_cast<const uint8_t *>(data.constData());

    // rtp version = 2
    if (((p[0] >> 6) & 0x03) != 2)
        return;

    // ignore all non-opus packets. theres rtcp and other stuff
    uint8_t payloadType = p[1] & 0x7F;
    if (payloadType != 120)
        return;

    // rtp header and extension header are unencrypted and used for aad in rtpsize
    bool hasExtension = (p[0] >> 4) & 1;
    int headerSize = RtpHeader::FIXED_SIZE + (hasExtension ? 4 : 0);

    if (data.size() <= headerSize + 4)
        return;

    RtpHeader header = RtpHeader::parse(data);
    QByteArray rtpHeaderBytes = data.left(headerSize);
    QByteArray encryptedSection = data.mid(headerSize);

    if (!encryption) {
        qCDebug(LogVoice) << "Received RTP but no encryption context, SSRC =" << header.ssrc;
        return;
    }

    QByteArray decrypted = encryption->decrypt(rtpHeaderBytes, encryptedSection);
    if (decrypted.isEmpty()) {
        qCDebug(LogVoice) << "Decrypt failed: SSRC =" << header.ssrc
                          << "seq =" << header.sequence
                          << "pktSize =" << data.size()
                          << "hdrSize =" << headerSize
                          << "ext =" << hasExtension
                          << "encSize =" << encryptedSection.size();
        return;
    }

    Core::Snowflake speaker = ssrcToUser.value(header.ssrc, Core::Snowflake::Invalid);
    qCDebug(LogVoice) << "Received audio: SSRC =" << header.ssrc
                      << "user =" << speaker
                      << "seq =" << header.sequence
                      << "ts =" << header.timestamp
                      << "payload =" << decrypted.size() << "bytes";
}

void VoiceClient::onSpeaking(const SpeakingData &data)
{
    if (data.userId->isValid() && data.ssrc != 0)
        ssrcToUser.insert(data.ssrc, data.userId);

    emit speakingReceived(data);
}

void VoiceClient::onClientConnect(const ClientConnectData &data)
{
    if (data.userId->isValid() && data.audioSsrc != 0)
        ssrcToUser.insert(data.audioSsrc, data.userId);

    emit clientConnected(data);
}

void VoiceClient::onClientDisconnect(Core::Snowflake userId)
{
    ssrcToUser.removeIf([userId](auto it) { return it.value() == userId; });

    emit clientDisconnected(userId);
}

void VoiceClient::onGatewayResumed()
{
    qCInfo(LogVoice) << "Voice session resumed, restoring to Connected state";

    // assume session intact if we could resume
    if (localSsrc != 0 && !sessionKey.isEmpty()) {
        setState(State::Connected);

        // just in case
        if (!encryption) {
            EncryptionMode mode = encryptionModeFromString(selectedMode);
            encryption = std::make_unique<VoiceEncryption>(mode, sessionKey);
        }

        sendSilence();
        if (!keepaliveTimer) {
            keepaliveTimer = new QTimer(this);
            connect(keepaliveTimer, &QTimer::timeout, this, &VoiceClient::sendSilence);
        }
        if (!keepaliveTimer->isActive())
            keepaliveTimer->start(KEEPALIVE_INTERVAL_MS);
    }
}

void VoiceClient::onIpDiscovered(const QString &ip, int port)
{
    qCInfo(LogVoice) << "IP Discovery: external" << ip << ":" << port;

    setState(State::SelectingProtocol);

    gateway->sendSelectProtocol(ip, port, selectedMode);

    setState(State::WaitingForSession);
}

void VoiceClient::onIpDiscoveryFailed(const QString &error)
{
    qCCritical(LogVoice) << "IP Discovery failed:" << error;
    stop();
}

Core::Snowflake VoiceClient::userIdForSsrc(quint32 ssrc) const
{
    return ssrcToUser.value(ssrc, Core::Snowflake::Invalid);
}

void VoiceClient::setState(State state)
{
    if (currentState == state)
        return;

    qCDebug(LogVoice) << "VoiceClient state:" << currentState << "->" << state;
    currentState = state;
    emit stateChanged(state);
}

} // namespace AV
} // namespace Discord
} // namespace Acheron
