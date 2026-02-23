#include "VoiceClient.hpp"
#include "VoiceGateway.hpp"
#include "UdpTransport.hpp"
#include "VoiceEncryption.hpp"
#include "RtpPacket.hpp"

#include "DaveSession.hpp"

#include "Core/AV/IAudioBackend.hpp"
#include "Core/Logging.hpp"

namespace Acheron {
namespace Discord {
namespace AV {

// 20ms at 48khz
static constexpr uint32_t OPUS_FRAME_SAMPLES = 960;

static QString formatDisplayableCode(const std::vector<uint8_t> &data, int bytesToConsume = 30, int groupSize = 5)
{
    int numGroups = bytesToConsume / groupSize;
    if (static_cast<int>(data.size()) < bytesToConsume)
        return {};

    uint64_t modulus = 1;
    for (int i = 0; i < groupSize; i++)
        modulus *= 10;

    QString result;
    for (int g = 0; g < numGroups; g++) {
        uint64_t value = 0;
        for (int b = 0; b < groupSize; b++)
            value = (value << 8) | data[g * groupSize + b];
        value %= modulus;

        if (g > 0)
            result += QLatin1Char(' ');
        result += QStringLiteral("%1").arg(value, groupSize, 10, QLatin1Char('0'));
    }
    return result;
}

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

void VoiceClient::seedConnectedUsers(const QList<Core::Snowflake> &userIds)
{
    for (auto id : userIds)
        connectedUserIds.insert(std::to_string(id));
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
    connect(gateway, &VoiceGateway::clientsConnected, this, &VoiceClient::onClientsConnect);
    connect(gateway, &VoiceGateway::clientDisconnected, this, &VoiceClient::onClientDisconnect);
    connect(gateway, &VoiceGateway::resumed, this, &VoiceClient::onGatewayResumed);

    connect(gateway, &VoiceGateway::binaryPayloadReceived,
            this, [this](int opcode, const QByteArray &payload) {
                if (!daveSession)
                    return;
                switch (opcode) {
                case static_cast<int>(VoiceOpCode::DAVE_MLS_EXTERNAL_SENDER_PACKAGE):
                    daveSession->onExternalSenderPackage(payload);
                    break;
                case static_cast<int>(VoiceOpCode::DAVE_MLS_PROPOSALS):
                    daveSession->onProposals(payload);
                    break;
                case static_cast<int>(VoiceOpCode::DAVE_MLS_ANNOUNCE_COMMIT_TRANSITION):
                    daveSession->onAnnounceCommitTransition(payload);
                    break;
                case static_cast<int>(VoiceOpCode::DAVE_MLS_WELCOME):
                    daveSession->onWelcome(payload);
                    break;
                default:
                    qCDebug(LogVoice) << "Unhandled DAVE binary opcode:" << opcode;
                    break;
                }
            });
    connect(gateway, &VoiceGateway::daveTransitionPrepare,
            this, [this](int protocolVersion, int transitionId) {
                if (daveSession)
                    daveSession->onPrepareTransition(protocolVersion, transitionId);
            });
    connect(gateway, &VoiceGateway::daveTransitionExecute,
            this, [this](int transitionId) {
                if (daveSession)
                    daveSession->onExecuteTransition(transitionId);
            });
    connect(gateway, &VoiceGateway::daveEpochPrepare,
            this, [this](int protocolVersion, int epoch) {
                if (!daveSession && protocolVersion > 0) {
                    // mid-call upgrade attempt. return cuz onPrepareEpoch also reinits
                    ensureDaveSession(static_cast<uint16_t>(protocolVersion));
                    return;
                }
                if (daveSession)
                    daveSession->onPrepareEpoch(protocolVersion, epoch);
            });

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

    connectedUserIds.clear();
    ssrcToUserIdMap.clear();

    setState(State::Disconnected);
}

void VoiceClient::cleanupTransport()
{
    if (keepaliveTimer) {
        keepaliveTimer->stop();
        delete keepaliveTimer;
        keepaliveTimer = nullptr;
    }

    encryption.reset();

    daveSession.reset();

    delete udpTransport;
    udpTransport = nullptr;

    rtpSequence = 0;
    rtpTimestamp = 0;
    rtpEpoch = {};
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

    int daveVersion = desc.daveProtocolVersion.hasValue() ? desc.daveProtocolVersion.get() : 0;
    qCInfo(LogVoice) << "dave_protocol_version =" << daveVersion;

    if (daveVersion > 0)
        ensureDaveSession(static_cast<uint16_t>(daveVersion));

    rtpEpoch = std::chrono::steady_clock::now();

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

void VoiceClient::sendAudio(const QByteArray &opusData)
{
    if (currentState != State::Connected || !encryption || !udpTransport)
        return;

    // snap rtp timestamp back to wall clock after a period of silence
    // otherwise its a little behind and it will be played back delayed by discord
    auto now = std::chrono::steady_clock::now();
    if (newTalkspurt) {
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - rtpEpoch);
        rtpTimestamp = static_cast<uint32_t>(static_cast<uint64_t>(elapsed.count()) * 48 / 1000);
    } else {
        rtpTimestamp += OPUS_FRAME_SAMPLES;
    }

    RtpHeader header;
    header.payloadType = 120;
    header.marker = newTalkspurt;
    header.sequence = rtpSequence++;
    header.timestamp = rtpTimestamp;
    header.ssrc = localSsrc;

    newTalkspurt = false;

    QByteArray payloadForTransport = opusData;

    if (isDaveEnabled()) {
        auto *enc = daveSession->encryptor();
        auto maxSize = enc->GetMaxCiphertextByteSize(discord::dave::MediaType::Audio, payloadForTransport.size());
        std::vector<uint8_t> daveEncrypted(maxSize);
        size_t bytesWritten = 0;
        auto result = enc->Encrypt(
                discord::dave::MediaType::Audio,
                localSsrc,
                discord::dave::ArrayView<const uint8_t>(
                        reinterpret_cast<const uint8_t *>(payloadForTransport.constData()),
                        payloadForTransport.size()),
                discord::dave::ArrayView<uint8_t>(daveEncrypted.data(), daveEncrypted.size()),
                &bytesWritten);

        if (result == discord::dave::IEncryptor::Success) {
            payloadForTransport = QByteArray(reinterpret_cast<const char *>(daveEncrypted.data()), bytesWritten);
        } else {
            qCWarning(LogVoice) << "DAVE encrypt failed: result =" << static_cast<int>(result);
            return;
        }
    }

    QByteArray headerBytes = header.serialize();
    QByteArray encryptedSection = encryption->encrypt(headerBytes, payloadForTransport);
    if (encryptedSection.isEmpty())
        return;

    QByteArray packet = headerBytes + encryptedSection;
    udpTransport->send(packet);

    lastAudioSendTime = now;
}

void VoiceClient::setSpeaking(bool speaking)
{
    if (!gateway)
        return;

    if (speaking)
        newTalkspurt = true;

    int flags = speaking ? static_cast<int>(SpeakingFlag::MICROPHONE) : 0;
    gateway->sendSpeaking(flags, 0, localSsrc);
}

void VoiceClient::sendSilence()
{
    if (currentState != State::Connected || !encryption || !udpTransport)
        return;

    // no need to keepalive by sending silence if we spoke recently
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastAudioSendTime);
    if (elapsed.count() < 5)
        return;

    QByteArray silencePayload(reinterpret_cast<const char *>(Core::AV::OPUS_SILENCE), sizeof(Core::AV::OPUS_SILENCE));
    sendAudio(silencePayload);

    qCDebug(LogVoice) << "Sent keepalive silence frame";
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

    // too small to contain meaningful audio after encryption overhead
    if (data.size() < 44)
        return;

    // rtp header and extension header are unencrypted and used for aad in rtpsize.
    // account for CSRC entries between fixed header and extension header.
    int csrcCount = p[0] & 0x0F;
    bool hasExtension = (p[0] >> 4) & 1;
    int headerSize = RtpHeader::FIXED_SIZE + csrcCount * 4 + (hasExtension ? 4 : 0);

    if (data.size() <= headerSize + 4)
        return;

    RtpHeader header = RtpHeader::parse(data);

    // ignore our own packets
    if (header.ssrc == localSsrc)
        return;

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

    if (hasExtension) {
        int extOffset = RtpHeader::FIXED_SIZE + csrcCount * 4;
        if (data.size() < extOffset + 4)
            return;
        uint16_t extWords = (p[extOffset + 2] << 8) | p[extOffset + 3];
        int extBytes = extWords * 4;
        if (decrypted.size() <= extBytes)
            return;
        decrypted = decrypted.mid(extBytes);
    }

    if (daveSession) {
        // https://daveprotocol.com/#silence-packets
        if (decrypted.size() == sizeof(Core::AV::OPUS_SILENCE) && memcmp(decrypted.constData(), Core::AV::OPUS_SILENCE, sizeof(Core::AV::OPUS_SILENCE)) == 0) {
            emit audioReceived(header.ssrc, header.sequence, header.timestamp, decrypted);
            return;
        }

        if (daveSession->isDaveEnabled()) {
            uint64_t ssrcUid = ssrcToUserIdMap.value(header.ssrc, 0);
            auto *dec = daveSession->getOrCreateDecryptor(header.ssrc, ssrcUid);
            auto maxSize = dec->GetMaxPlaintextByteSize(discord::dave::MediaType::Audio, decrypted.size());
            std::vector<uint8_t> davePlaintext(maxSize);
            size_t bytesWritten = 0;
            auto result = dec->Decrypt(
                    discord::dave::MediaType::Audio,
                    discord::dave::ArrayView<const uint8_t>(
                            reinterpret_cast<const uint8_t *>(decrypted.constData()),
                            decrypted.size()),
                    discord::dave::ArrayView<uint8_t>(davePlaintext.data(), davePlaintext.size()),
                    &bytesWritten);

            if (result == discord::dave::IDecryptor::Success) {
                decrypted = QByteArray(reinterpret_cast<const char *>(davePlaintext.data()), bytesWritten);
            } else {
                static constexpr const char *kResultNames[] = {
                    "Success",
                    "DecryptionFailure",
                    "MissingKeyRatchet",
                    "InvalidNonce",
                    "MissingCryptor",
                };
                int ri = static_cast<int>(result);
                const char *rn = (ri >= 0 && ri < 5) ? kResultNames[ri] : "Unknown";
                bool hasMagic = decrypted.size() >= 2 && static_cast<uint8_t>(decrypted[decrypted.size() - 2]) == 0xFA && static_cast<uint8_t>(decrypted[decrypted.size() - 1]) == 0xFA;
                qCDebug(LogDave) << "DAVE decrypt failed: SSRC =" << header.ssrc
                                 << "result =" << rn
                                 << "frameSize =" << decrypted.size()
                                 << "hasMagicMarker =" << hasMagic;
                return;
            }
        } else if (daveSession->isDowngraded()) {
            // fallthrough
        } else {
            return;
        }
    }

    emit audioReceived(header.ssrc, header.sequence, header.timestamp, decrypted);
}

bool VoiceClient::isDaveEnabled() const
{
    return daveSession && daveSession->isDaveEnabled();
}

void VoiceClient::requestVerificationCode(Core::Snowflake targetUserId,
                                          std::function<void(const QString &)> callback)
{
    if (!isDaveEnabled()) {
        callback(QString());
        return;
    }
    std::string uid = std::to_string(targetUserId);
    daveSession->getPairwiseFingerprint(uid,
                                        [cb = std::move(callback)](const std::vector<uint8_t> &fingerprint) {
                                            if (fingerprint.empty()) {
                                                cb(QString());
                                                return;
                                            }
                                            cb(formatDisplayableCode(fingerprint, 45));
                                        });
}

void VoiceClient::onSpeaking(const SpeakingData &data)
{
    if (data.userId.hasValue() && data.userId->isValid()) {
        std::string uid = std::to_string(data.userId.get());
        connectedUserIds.insert(uid);
        if (data.ssrc.get() != 0)
            ssrcToUserIdMap.insert(data.ssrc, data.userId.get());
        if (daveSession) {
            daveSession->addConnectedUser(uid);
            if (data.ssrc.get() != 0)
                daveSession->applyKeyRatchetForSsrc(data.ssrc, data.userId.get());
        }
    }
    emit speakingReceived(data);
}

void VoiceClient::onClientsConnect(const QStringList &userIds)
{
    for (const auto &id : userIds) {
        std::string uid = id.toStdString();
        connectedUserIds.insert(uid);
        if (daveSession)
            daveSession->addConnectedUser(uid);
    }
}

void VoiceClient::onClientConnect(const ClientConnectData &data)
{
    if (data.userId.hasValue() && data.userId->isValid()) {
        std::string uid = std::to_string(data.userId.get());
        connectedUserIds.insert(uid);
        if (data.audioSsrc.get() != 0)
            ssrcToUserIdMap.insert(data.audioSsrc, data.userId.get());
        if (daveSession)
            daveSession->addConnectedUser(uid);
    }
    emit clientConnected(data);
}

void VoiceClient::onClientDisconnect(Core::Snowflake uid)
{
    std::string uidStr = std::to_string(uid);
    connectedUserIds.erase(uidStr);

    if (daveSession)
        daveSession->removeConnectedUser(uidStr);

    for (auto it = ssrcToUserIdMap.begin(); it != ssrcToUserIdMap.end(); ++it) {
        if (it.value() == static_cast<quint64>(uid)) {
            ssrcToUserIdMap.erase(it);
            break;
        }
    }

    emit clientDisconnected(uid);
}

void VoiceClient::onGatewayResumed()
{
    qCInfo(LogVoice) << "Voice session resumed, restoring to Connected state";

    // assume session intact if we could resume
    if (localSsrc != 0 && !sessionKey.isEmpty()) {
        rtpEpoch = std::chrono::steady_clock::now();
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

void VoiceClient::ensureDaveSession(uint16_t protocolVersion)
{
    if (daveSession)
        return;

    qCInfo(LogVoice) << "Creating DAVE session, protocol version =" << protocolVersion;

    daveSession = std::make_unique<DaveSession>(channelId, userId, ssrcToUserIdMap, this);
    daveSession->setLocalSsrc(localSsrc);

    for (const auto &uid : connectedUserIds)
        daveSession->addConnectedUser(uid);

    // binary
    connect(daveSession.get(), &DaveSession::sendKeyPackage,
            gateway, &VoiceGateway::sendBinaryPayload);
    connect(daveSession.get(), &DaveSession::sendCommitWelcome,
            gateway, &VoiceGateway::sendBinaryPayload);

    // json
    connect(daveSession.get(), &DaveSession::sendReadyForTransition,
            gateway, &VoiceGateway::sendDaveReadyForTransition);
    connect(daveSession.get(), &DaveSession::sendInvalidCommitWelcome,
            gateway, &VoiceGateway::sendDaveInvalidCommitWelcome);

    connect(daveSession.get(), &DaveSession::daveStateChanged,
            this, [this](bool enabled) {
                if (enabled) {
                    auto auth = daveSession->lastEpochAuthenticator();
                    if (!auth.empty())
                        emit privacyCodeChanged(formatDisplayableCode(auth));
                    else
                        emit privacyCodeChanged(QString());
                } else {
                    emit privacyCodeChanged(QString());
                }
            });

    daveSession->init(protocolVersion);
}

void VoiceClient::setState(State state)
{
    if (currentState == state)
        return;

    qCDebug(LogVoice) << "VoiceClient state:" << currentState.load() << "->" << state;
    currentState = state;
    emit stateChanged(state);
}

} // namespace AV
} // namespace Discord
} // namespace Acheron
