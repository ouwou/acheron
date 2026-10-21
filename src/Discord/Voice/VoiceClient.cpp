#include "VoiceClient.hpp"
#include "VoiceGateway.hpp"
#include "UdpTransport.hpp"
#include "VoiceEncryption.hpp"
#include "RtpPacket.hpp"
#include "Rtcp.hpp"
#include "VideoReceivePath.hpp"
#include "VideoSendPath.hpp"

#include "DaveSession.hpp"

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Logging.hpp"

#include <QUuid>

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
    full.quality = FULL_QUALITY;
    if (!withSecondLayer)
        return { full };

    StreamDescription second = full;
    second.rid = QStringLiteral("50");
    second.quality = SECOND_LAYER_QUALITY;
    return { full, second };
}

VoiceClient::VoiceClient(const ConnectionParams &params, QObject *parent)
    : QObject(parent), params(params)
{
    // emitted across threads. Qt 6 registers these itself; Qt 5 drops them unregistered
    qRegisterMetaType<VoiceCloseCode>();
    qRegisterMetaType<VoiceClient::State>();
    qRegisterMetaType<Core::Snowflake>();
    qRegisterMetaType<SpeakingData>();
    qRegisterMetaType<VideoReceiveStats>();
    qRegisterMetaType<VideoSendStats>();
    qRegisterMetaType<VideoCodec>();
    qRegisterMetaType<QList<VideoLayer>>();

    if (params.video) {
        receivedVideo = std::make_unique<VideoReceivePath>(*this);
        sentVideo = std::make_unique<VideoSendPath>(*this);
    }
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
    if (sentVideo)
        sentVideo = std::make_unique<VideoSendPath>(*this);
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

    if (receivedVideo)
        receivedVideo->reset();
    if (sentVideo)
        sentVideo->reset();

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
    if (sentVideo)
        sentVideo->takeStreams(data);
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

    if (sentVideo)
        sentVideo->setServerCodec(desc.videoCodec.valueOr(QString()));

    setState(State::Connected);
    emit connected();

    if (std::exchange(codecsChangedSinceOffered, false))
        sendOfferedCodecs();

    if (receivedVideo)
        receivedVideo->onConnected();

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
    if (!canSendMedia() || !params.sendsAudio)
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
    const int headerSize = writeRtpHeader(header, packet->writable());
    packet->resize(headerSize);

    const std::span<const uint8_t> opus(reinterpret_cast<const uint8_t *>(opusData.constData()), static_cast<size_t>(opusData.size()));

    if (isDaveEnabled()) {
        const size_t maxSize = daveSession->maxCiphertextSize(discord::dave::MediaType::Audio, opus.size());
        if (static_cast<int>(maxSize) > packet->tailroom() - VoiceEncryption::TRAILER_SIZE)
            return;

        const std::optional<size_t> written = daveSession->encrypt(discord::dave::MediaType::Audio, localSsrc, opus, { packet->data() + headerSize, maxSize });
        if (!written)
            return;
        packet->resize(headerSize + static_cast<int>(*written));
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
    if (!canSendMedia())
        return;

    // a stream connection has no microphone, so no opening silence
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

    if (receivedVideo && receivedVideo->receive(*rtp, packet))
        return;

    // the server numbers its link-probing padding too, and an unreported packet counts as lost
    (void)openRtpPayload(*rtp, *packet);
    unrouted.rtp++;
}

void VoiceClient::receiveRtcp(PacketPool::Handle packet)
{
    if (!encryption || !encryption->open(*packet, RTCP_CLEAR_SIZE)) {
        unrouted.rtcp++;
        return;
    }

    const ReceivedRtcp received = parseRtcp(packet->bytes());
    if (sentVideo)
        sentVideo->receiveRtcp(received);
    if (receivedVideo)
        receivedVideo->receiveRtcp(received);
    for (const ReceptionReport &report : received.receptionReports) {
        if (report.ssrc == localSsrc)
            qCDebug(LogVoice) << "Server's report on our audio: has up to sequence" << (report.highestSequence & 0xFFFF) << "of" << static_cast<uint16_t>(rtpSequence - 1) << "sent, lost in all" << report.cumulativeLost << "lost lately" << report.fractionLost / 256.0;
    }
}

void VoiceClient::noteTransportSequence(std::span<const uint8_t> extensions)
{
    if (receivedVideo)
        receivedVideo->noteTransportSequence(extensions);
}

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

void VoiceClient::offerVideoEncoding(const QList<VideoCodec> &encodable)
{
    bool changed = false;
    for (Codec &offered : params.videoCodecs) {
        const bool encoded = encodable.contains(videoCodecFromName(offered.name));
        if (offered.encode.valueOr(false) == encoded)
            continue;
        offered.encode = encoded;
        changed = true;
    }
    if (!changed)
        return;

    if (currentState == State::WaitingForSession)
        codecsChangedSinceOffered = true;
    if (currentState != State::Connected)
        return;

    sendOfferedCodecs();
    if (sentVideo)
        sentVideo->retakeServerCodec();
}

std::function<void(VideoCodec)> VoiceClient::queuedCodecWithdrawal()
{
    return [this](VideoCodec codec) { QMetaObject::invokeMethod(this, [this, codec] { withdrawVideoCodec(codec); }); };
}

void VoiceClient::withdrawVideoCodec(VideoCodec codec)
{
    bool changed = false;
    for (Codec &offered : params.videoCodecs) {
        if (videoCodecFromName(offered.name) != codec || !offered.decode.valueOr(false))
            continue;
        offered.decode = false;
        changed = true;
    }
    if (!changed || !gateway || currentState != State::Connected)
        return;

    qCWarning(LogVoice) << "Withdrawing" << videoCodecName(codec) << "from the codecs this client decodes";
    sendOfferedCodecs();
}

void VoiceClient::setVideoFrameHandler(VideoFrameHandler handler)
{
    if (receivedVideo)
        receivedVideo->setFrameHandler(std::move(handler));
}

void VoiceClient::setVideoWants(int pixelCount, bool wanted)
{
    if (receivedVideo)
        receivedVideo->setWants(pixelCount, wanted);
}

void VoiceClient::setVideoPlan(const MediaSinkWants &wants, const QSet<quint32> &deliver)
{
    if (receivedVideo)
        receivedVideo->setPlan(wants, deliver);
}

void VoiceClient::requestVideoKeyframeFrom(Core::Snowflake userId)
{
    if (receivedVideo)
        receivedVideo->requestKeyframeFrom(userId);
}

void VoiceClient::requestVideoKeyframeFromEveryone()
{
    if (receivedVideo)
        receivedVideo->requestKeyframeFromEveryone();
}

VideoCodec VoiceClient::videoSendCodec() const
{
    return sentVideo ? sentVideo->codec() : VideoCodec::Unknown;
}

void VoiceClient::startVideo(const LocalVideoDescription &description)
{
    if (sentVideo)
        sentVideo->start(description);
}

void VoiceClient::describeVideo(const LocalVideoDescription &description)
{
    if (sentVideo)
        sentVideo->describe(description);
}

void VoiceClient::setVideoBitrate(int bitsPerSecond, int quality)
{
    if (sentVideo)
        sentVideo->setBitrate(bitsPerSecond, quality);
}

void VoiceClient::stopVideo()
{
    if (sentVideo)
        sentVideo->stop();
}

void VoiceClient::sendVideoFrame(const EncodedVideoFrame &frame, int quality)
{
    if (sentVideo)
        sentVideo->sendFrame(frame, quality);
}

void VoiceClient::onSessionUpdated(const SessionUpdate &update)
{
    if (sentVideo && update.videoCodec.hasValue())
        sentVideo->setServerCodec(update.videoCodec.get());
}

void VoiceClient::onMediaSinkWants(const MediaSinkWants &wants)
{
    if (sentVideo)
        sentVideo->onMediaSinkWants(wants);
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

    if (!openRtpPayload(rtp, *packet) || packet->size() == 0)
        return;

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

bool VoiceClient::openRtpPayload(const ParsedRtp &rtp, PacketBuffer &packet)
{
    if (!encryption || !encryption->open(packet, rtp.clearSize)) {
        qCDebug(LogVoice) << "Decrypt failed: SSRC =" << rtp.header.ssrc << "seq =" << rtp.header.sequence << "hdrSize =" << rtp.clearSize << "ext =" << rtp.header.extension;
        return false;
    }

    // the extension elements are encrypted with the payload and come out in front of it
    packet.dropFront(rtp.clearSize);
    if (packet.size() < rtp.extensionSize)
        return false;
    noteTransportSequence(packet.bytes().first(static_cast<size_t>(rtp.extensionSize)));
    packet.dropFront(rtp.extensionSize);

    if (!rtp.header.padding)
        return true;
    const std::optional<int> padding = rtpPaddingSize(packet.bytes());
    return padding && packet.resize(packet.size() - *padding);
}

bool VoiceClient::daveDecryptAudio(quint32 ssrc, PacketPool::Handle &packet)
{
    PacketPool::Handle plaintext = udpTransport->acquirePacket();
    if (!plaintext->resize(static_cast<int>(daveSession->maxPlaintextSize(discord::dave::MediaType::Audio, ssrc, packet->size()))))
        return false;

    const std::optional<size_t> written = daveSession->decrypt(discord::dave::MediaType::Audio, ssrc, packet->bytes(), plaintext->bytes());
    if (!written)
        return false;
    plaintext->resize(static_cast<int>(*written));
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
    if (receivedVideo)
        receivedVideo->onClientConnect(data);
    emit clientConnected(data);
}

void VoiceClient::onClientDisconnect(Core::Snowflake uid)
{
    std::string uidStr = std::to_string(uid);
    connectedUserIds.erase(uidStr);

    if (daveSession)
        daveSession->removeConnectedUser(uidStr);

    if (receivedVideo)
        receivedVideo->onClientDisconnect(uid);

    for (auto it = ssrcToUserIdMap.begin(); it != ssrcToUserIdMap.end();) {
        if (it.value() == static_cast<quint64>(uid))
            it = ssrcToUserIdMap.erase(it);
        else
            ++it;
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
    if (sentVideo)
        sentVideo->registerWithDave();

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

    connect(daveSession.get(), &DaveSession::newKeysNeedKeyframe, this, [this] {
        if (sentVideo)
            sentVideo->askEveryLayerForKeyframe();
    });

    connect(daveSession.get(), &DaveSession::daveStateChanged,
            this, [this](bool enabled) {
                if (receivedVideo)
                    receivedVideo->onDaveStateChanged();
                if (sentVideo)
                    sentVideo->askAwaitingLayersForKeyframe();
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
