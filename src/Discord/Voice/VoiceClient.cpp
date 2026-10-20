#include "VoiceClient.hpp"
#include "VoiceGateway.hpp"
#include "UdpTransport.hpp"
#include "VoiceEncryption.hpp"
#include "RtpPacket.hpp"
#include "Rtcp.hpp"
#include "VideoSendStream.hpp"

#include "DaveSession.hpp"

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Logging.hpp"

#include <QDateTime>
#include <QRandomGenerator>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <utility>

namespace Acheron {
namespace Discord {
namespace Voice {

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

QList<StreamDescription> cameraStreams(bool withSecondLayer)
{
    StreamDescription full;
    full.type = QStringLiteral("video");
    full.rid = QStringLiteral("100");
    full.quality = 100;
    if (!withSecondLayer)
        return { full };

    StreamDescription second = full;
    second.rid = QStringLiteral("50");
    second.quality = 50;
    return { full, second };
}

VoiceClient::VoiceClient(const ConnectionParams &params, QObject *parent)
    : QObject(parent), params(params)
{
    qRegisterMetaType<VideoReceiveStats>();
    qRegisterMetaType<VideoSendStats>();
    qRegisterMetaType<VideoCodec>();
    qRegisterMetaType<QList<VideoLayer>>();

    readVideoPayloadTypes();
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

    gateway = new VoiceGateway(params, this);

    connect(gateway, &VoiceGateway::connected, this, &VoiceClient::onGatewayConnected);
    connect(gateway, &VoiceGateway::disconnected, this, &VoiceClient::onGatewayDisconnected);
    connect(gateway, &VoiceGateway::readyReceived, this, &VoiceClient::onGatewayReady);
    connect(gateway, &VoiceGateway::sessionDescriptionReceived, this, &VoiceClient::onSessionDescription);
    connect(gateway, &VoiceGateway::sessionUpdated, this, &VoiceClient::onSessionUpdated);
    connect(gateway, &VoiceGateway::mediaSinkWantsReceived, this, &VoiceClient::onMediaSinkWants);
    connect(gateway, &VoiceGateway::speakingReceived, this, &VoiceClient::onSpeaking);
    connect(gateway, &VoiceGateway::clientConnected, this, &VoiceClient::onClientConnect);
    connect(gateway, &VoiceGateway::clientsConnected, this, &VoiceClient::onClientsConnect);
    connect(gateway, &VoiceGateway::clientDisconnected, this, &VoiceClient::onClientDisconnect);
    connect(gateway, &VoiceGateway::resumed, this, &VoiceClient::onGatewayResumed);
    connect(gateway, &VoiceGateway::reconnecting, this, &VoiceClient::reconnecting);

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

    if (unrouted.rtcp != 0 || unrouted.rtp != 0)
        qCDebug(LogVoice) << "Packets received with no consumer: RTCP" << unrouted.rtcp << "RTP" << unrouted.rtp;
    unrouted = {};

    localSsrc = 0;
    localVideoStreams.clear();
    localLayers.clear();
    sendCodec = VideoCodec::Unknown;
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

    // The receivers hold packets from the transport's pool.
    cleanupVideo();

    encryption.reset();

    daveSession.reset();

    delete udpTransport;
    udpTransport = nullptr;

    rtpSequence = 0;
    rtpTimestamp = 0;
    rtpEpoch = {};
    soundshareAnnounced = false;
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
    takeLocalVideoStreams(data);
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

    if (params.video)
        gateway->sendVoiceBackendVersion();

    cleanupTransport();

    udpTransport = new UdpTransport(params.proxy, this);
    connect(udpTransport, &UdpTransport::ipDiscovered, this, &VoiceClient::onIpDiscovered);
    connect(udpTransport, &UdpTransport::ipDiscoveryFailed, this, &VoiceClient::onIpDiscoveryFailed);
    connect(udpTransport, &UdpTransport::relayLost, this, &VoiceClient::onRelayLost);
    udpTransport->setPacketHandler([this](PacketPool::Handle packet) { onPacket(std::move(packet)); });

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

    // Whoever starts video from connected() has to find the codec already known.
    setVideoSendCodec(desc.videoCodec.valueOr(QString()));

    setState(State::Connected);
    emit connected();

    if (std::exchange(codecsChangedSinceOffered, false))
        sendOfferedCodecs();

    if (!videoPayloadTypes.isEmpty())
        startVideoTimer();
    sendVideoWants();

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
    if (currentState != State::Connected || !encryption || !udpTransport || !params.sendsAudio)
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
    header.payloadType = Codec::OPUS_PAYLOAD_TYPE;
    header.marker = newTalkspurt;
    header.sequence = rtpSequence++;
    header.timestamp = rtpTimestamp;
    header.ssrc = localSsrc;

    newTalkspurt = false;

    PacketPool::Handle packet = udpTransport->acquirePacket();
    const int headerSize = writeRtpHeader(header, { packet->data(), static_cast<size_t>(packet->tailroom()) });
    packet->resize(headerSize);

    const std::span<const uint8_t> opus(reinterpret_cast<const uint8_t *>(opusData.constData()), static_cast<size_t>(opusData.size()));

    if (isDaveEnabled()) {
        auto *enc = daveSession->encryptor();
        const size_t maxSize = enc->GetMaxCiphertextByteSize(discord::dave::MediaType::Audio, opus.size());
        if (static_cast<int>(maxSize) > packet->tailroom() - VoiceEncryption::TRAILER_SIZE)
            return;

        size_t bytesWritten = 0;
        auto result = enc->Encrypt(discord::dave::MediaType::Audio, localSsrc, discord::dave::ArrayView<const uint8_t>(opus.data(), opus.size()), discord::dave::ArrayView<uint8_t>(packet->data() + headerSize, maxSize), &bytesWritten);
        if (result != discord::dave::IEncryptor::Success) {
            qCWarning(LogVoice) << "DAVE encrypt failed: result =" << static_cast<int>(result);
            return;
        }
        packet->resize(headerSize + static_cast<int>(bytesWritten));
    } else if (!packet->append(opus)) {
        return;
    }

    if (!encryption->seal(*packet, headerSize))
        return;

    udpTransport->send(*packet);

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

void VoiceClient::beginSoundshare()
{
    if (currentState != State::Connected || !gateway || !params.sendsAudio)
        return;

    newTalkspurt = true;
    if (std::exchange(soundshareAnnounced, true))
        return;
    gateway->sendSpeaking(static_cast<int>(SpeakingFlag::SOUNDSHARE), 0, localSsrc);
}

void VoiceClient::sendSilence()
{
    if (currentState != State::Connected || !encryption || !udpTransport)
        return;

    // The silence is the microphone's, and a stream's connection has no microphone: its owner
    // sends the sound it shares and nothing in between.
    if (params.streamContext)
        return;

    // no need to keepalive by sending silence if we spoke recently
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastAudioSendTime);
    if (elapsed.count() < 5)
        return;

    QByteArray silencePayload(reinterpret_cast<const char *>(Core::Audio::OPUS_SILENCE), sizeof(Core::Audio::OPUS_SILENCE));
    sendAudio(silencePayload);

    qCDebug(LogVoice) << "Sent keepalive silence frame";
}

void VoiceClient::onPacket(PacketPool::Handle packet)
{
    switch (classifyPacket(packet->bytes())) {
    case PacketKind::Rtcp:
        receiveRtcp(std::move(packet));
        return;
    case PacketKind::Unknown:
        return;
    case PacketKind::Rtp:
        break;
    }

    const std::optional<ParsedRtp> rtp = parseRtp(packet->bytes());
    if (!rtp)
        return;

    const int payloadType = rtp->header.payloadType;
    if (payloadType == Codec::OPUS_PAYLOAD_TYPE) {
        receiveAudio(*rtp, std::move(packet));
        return;
    }

    if (auto video = videoPayloadTypes.constFind(payloadType); video != videoPayloadTypes.constEnd()) {
        receiveVideo(*rtp, std::move(packet), video.value(), false);
        return;
    }
    if (auto rtx = rtxPayloadTypes.constFind(payloadType); rtx != rtxPayloadTypes.constEnd()) {
        receiveVideo(*rtp, std::move(packet), rtx.value(), true);
        return;
    }

    // Nothing this client plays, but the server numbers these too: it tries the link with
    // padding under a payload type of its own, and a packet not reported counts as lost.
    if (encryption && encryption->open(*packet, rtp->clearSize)) {
        packet->dropFront(rtp->clearSize);
        if (packet->size() >= rtp->extensionSize)
            noteTransportSequence(packet->bytes().first(static_cast<size_t>(rtp->extensionSize)));
    }
    unrouted.rtp++;
}

void VoiceClient::receiveRtcp(PacketPool::Handle packet)
{
    if (!encryption || !encryption->open(*packet, RTCP_CLEAR_SIZE)) {
        unrouted.rtcp++;
        return;
    }

    const ReceivedRtcp received = parseRtcp(packet->bytes());
    receiveVideoFeedback(received);
    for (uint32_t ssrc : received.videoResets)
        receiveVideoAfresh(ssrc);
    if (videoClock.isValid()) {
        const qint64 now = videoClock.nsecsElapsed() / 1000;
        for (const ReceivedRtcp::SenderReport &report : received.senderReports)
            senderReports.insert(report.ssrc, { report.compactNtp, now });
    }
    for (const ReceptionReport &report : received.receptionReports) {
        // Whether what this client sends is arriving is otherwise only known to the people
        // who cannot hear it.
        if (report.ssrc == localSsrc)
            qCDebug(LogVoice) << "Server's report on our audio: has up to sequence" << (report.highestSequence & 0xFFFF) << "of" << static_cast<uint16_t>(rtpSequence - 1) << "sent, lost in all" << report.cumulativeLost << "lost lately" << report.fractionLost / 256.0;
    }
    if (received.hasInboundEstimate) {
        if (received.inboundBitsPerSecond != serverEstimateBps)
            qCDebug(LogVoice) << "Server's estimate of the way here:" << received.inboundBitsPerSecond << "bps, loss" << received.inboundLoss;
        serverEstimateBps = received.inboundBitsPerSecond;
        serverLossFraction = received.inboundLoss;
    }
}

// A video codec that is neither encoded nor decoded any more is left out, as the official
// client leaves it out.
QList<Codec> VoiceClient::offeredCodecs() const
{
    QList<Codec> codecs = { Codec::opus() };
    for (const Codec &video : params.videoCodecs) {
        if (video.encode.valueOr(false) || video.decode.valueOr(false))
            codecs.append(video);
    }
    return codecs;
}

void VoiceClient::sendOfferedCodecs()
{
    gateway->sendSessionUpdate(offeredCodecs());
}

void VoiceClient::receiveAudio(const ParsedRtp &rtp, PacketPool::Handle packet)
{
    const RtpHeader &header = rtp.header;

    // ignore our own packets
    if (header.ssrc == localSsrc)
        return;

    if (!encryption) {
        qCDebug(LogVoice) << "Received RTP but no encryption context, SSRC =" << header.ssrc;
        return;
    }

    const int packetSize = packet->size();
    if (!encryption->open(*packet, rtp.clearSize)) {
        qCDebug(LogVoice) << "Decrypt failed: SSRC =" << header.ssrc
                          << "seq =" << header.sequence
                          << "pktSize =" << packetSize
                          << "hdrSize =" << rtp.clearSize
                          << "ext =" << header.extension;
        return;
    }

    // the extension elements are encrypted with the payload and come out in front of it
    packet->dropFront(rtp.clearSize);
    if (packet->size() <= rtp.extensionSize)
        return;
    noteTransportSequence(packet->bytes().first(static_cast<size_t>(rtp.extensionSize)));
    packet->dropFront(rtp.extensionSize);

    // Stream audio arrives padded now and then; the padding is not part of the frame.
    if (header.padding) {
        const int padding = rtpPaddingSize(packet->bytes());
        if (padding < 0 || padding == packet->size())
            return;
        packet->resize(packet->size() - padding);
    }

    if (daveSession) {
        // https://daveprotocol.com/#silence-packets
        const bool isSilence = packet->size() == sizeof(Core::Audio::OPUS_SILENCE) && memcmp(packet->data(), Core::Audio::OPUS_SILENCE, sizeof(Core::Audio::OPUS_SILENCE)) == 0;

        if (!isSilence) {
            if (daveSession->isDaveEnabled()) {
                if (!daveDecryptAudio(header.ssrc, packet))
                    return;
            } else if (!daveSession->isDowngraded()) {
                return;
            }
        }
    }

    emit audioReceived(header.ssrc, header.sequence, header.timestamp, QByteArray(reinterpret_cast<const char *>(packet->data()), packet->size()));
}

bool VoiceClient::daveDecryptAudio(quint32 ssrc, PacketPool::Handle &packet)
{
    uint64_t ssrcUid = ssrcToUserIdMap.value(ssrc, 0);
    auto *dec = daveSession->getOrCreateDecryptor(ssrc, ssrcUid);

    const size_t maxSize = dec->GetMaxPlaintextByteSize(discord::dave::MediaType::Audio, packet->size());
    PacketPool::Handle plaintext = udpTransport->acquirePacket();
    if (!plaintext->resize(static_cast<int>(maxSize)))
        return false;

    size_t bytesWritten = 0;
    auto result = dec->Decrypt(discord::dave::MediaType::Audio, discord::dave::ArrayView<const uint8_t>(packet->data(), packet->size()), discord::dave::ArrayView<uint8_t>(plaintext->data(), maxSize), &bytesWritten);

    if (result != discord::dave::IDecryptor::Success) {
        static constexpr const char *kResultNames[] = {
            "Success",
            "DecryptionFailure",
            "MissingKeyRatchet",
            "InvalidNonce",
            "MissingCryptor",
        };
        int ri = static_cast<int>(result);
        const char *rn = (ri >= 0 && ri < 5) ? kResultNames[ri] : "Unknown";
        const std::span<const uint8_t> frame = packet->bytes();
        bool hasMagic = frame.size() >= 2 && frame[frame.size() - 2] == 0xFA && frame[frame.size() - 1] == 0xFA;
        qCDebug(LogDave) << "DAVE decrypt failed: SSRC =" << ssrc
                         << "result =" << rn
                         << "frameSize =" << packet->size()
                         << "hasMagicMarker =" << hasMagic;
        return false;
    }

    plaintext->resize(static_cast<int>(bytesWritten));
    packet = std::move(plaintext);
    return true;
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
        emit clientReachedServer(Core::Snowflake(id.toULongLong()));
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
    applyRemoteVideo(data);
    emit clientConnected(data);
}

void VoiceClient::onClientDisconnect(Core::Snowflake uid)
{
    std::string uidStr = std::to_string(uid);
    connectedUserIds.erase(uidStr);

    if (daveSession)
        daveSession->removeConnectedUser(uidStr);

    const bool hadVideo = remoteLayers.contains(uid);
    removeRemoteVideo(uid);
    if (hadVideo) {
        emit remoteVideoLayersChanged(uid, {});
        emit remoteVideoChanged(uid, false, 0, 0, 0);
    }

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

    if (localSsrc == 0 || sessionKey.isEmpty() || !encryption || !udpTransport) {
        cleanupTransport();
        if (currentState != State::Disconnected) {
            setState(State::Disconnected);
            emit disconnected();
        }
        return;
    }

    rtpEpoch = std::chrono::steady_clock::now();
    setState(State::Connected);

    sendSilence();
    if (!keepaliveTimer) {
        keepaliveTimer = new QTimer(this);
        connect(keepaliveTimer, &QTimer::timeout, this, &VoiceClient::sendSilence);
    }
    if (!keepaliveTimer->isActive())
        keepaliveTimer->start(KEEPALIVE_INTERVAL_MS);
}

void VoiceClient::onIpDiscovered(const QString &ip, int port)
{
    qCInfo(LogVoice) << "IP Discovery: external" << ip << ":" << port;

    setState(State::SelectingProtocol);

    SelectProtocolData protocol;
    protocol.address = ip;
    protocol.port = port;
    protocol.mode = selectedMode;
    protocol.codecs = offeredCodecs();
    codecsChangedSinceOffered = false;
    if (params.video)
        protocol.rtcConnectionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    gateway->sendSelectProtocol(protocol);

    setState(State::WaitingForSession);
}

void VoiceClient::onIpDiscoveryFailed(const QString &error)
{
    qCCritical(LogVoice) << "IP Discovery failed:" << error;
    stop();
}

void VoiceClient::onRelayLost(const QString &error)
{
    qCWarning(LogVoice) << "SOCKS5 relay lost, ending the connection:" << error;
    stop();
    emit disconnected();
}

void VoiceClient::ensureDaveSession(uint16_t protocolVersion)
{
    if (daveSession)
        return;

    qCInfo(LogVoice) << "Creating DAVE session, protocol version =" << protocolVersion;

    daveSession = std::make_unique<DaveSession>(params.channelId, params.userId, ssrcToUserIdMap, this);
    daveSession->setLocalSsrc(localSsrc);
    registerLocalVideoWithDave();

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

    connect(daveSession.get(), &DaveSession::newKeysNeedKeyframe, this, &VoiceClient::askForLocalKeyframe);

    connect(daveSession.get(), &DaveSession::daveStateChanged,
            this, [this](bool enabled) {
                if (std::exchange(videoDroppedAwaitingDaveKeys, false))
                    requestVideoKeyframeFromEveryone();
                askForAwaitedLocalKeyframes();
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

} // namespace Voice
} // namespace Discord
} // namespace Acheron
