#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "PacketBuffer.hpp"
#include "Rtcp.hpp"
#include "RtpPacket.hpp"
#include "SinkWants.hpp"
#include "VideoFrame.hpp"
#include "VoiceEnums.hpp"
#include "VoiceEntities.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

class VoiceGateway;
class UdpTransport;
class VoiceEncryption;
class DaveSession;

struct VideoReceiveStats
{
    QString codec;
    quint64 packets = 0;
    quint64 bytes = 0;
    quint64 frames = 0;
    quint64 keyframes = 0;
    quint64 retransmitted = 0;
    quint64 nacked = 0;
    quint64 keyframeRequests = 0;
    quint64 decryptFailures = 0;
    double lossFraction = 0;
    std::optional<InboundEstimate> serverEstimate;
};

struct LocalVideoDescription
{
    std::optional<QSize> maxSize;
    int maxFramerate = 30;
    int maxBitrateBps = 0;
    std::optional<int> secondLayerMaxBitrateBps;
};

struct VideoSendStats
{
    QString codec;
    quint64 packets = 0;
    quint64 bytes = 0;
    quint64 frames = 0;
    quint64 keyframes = 0;
    quint64 retransmitted = 0;
    quint64 nacksReceived = 0;
    quint64 keyframeRequests = 0;
    quint64 bitrateBps = 0;
    std::optional<double> lossFraction;
    std::optional<int> roundTripMs;
    std::optional<int> receivers;
    std::optional<quint64> receiverEstimateBps;
    std::optional<quint64> delayEstimateBps;
    std::optional<quint64> acknowledgedBps;
};

// as the official client declares with simulcast on. a viewer of a single camera receives the second
[[nodiscard]] QList<StreamDescription> cameraStreams(bool withSecondLayer);

class VoiceClient : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Disconnected,
        Connecting,
        Identifying,
        WaitingForReady,
        DiscoveringIP,
        SelectingProtocol,
        WaitingForSession,
        Connected,
    };
    Q_ENUM(State)

    explicit VoiceClient(const ConnectionParams &params, QObject *parent = nullptr);
    ~VoiceClient() override;

    void start();
    void stop();

    [[nodiscard]] State state() const { return currentState; }
    [[nodiscard]] quint32 ssrc() const { return localSsrc; }
    [[nodiscard]] const QString &encryptionMode() const { return selectedMode; }
    [[nodiscard]] const QByteArray &secretKey() const { return sessionKey; }

    void seedConnectedUsers(const QList<Core::Snowflake> &userIds);

    void sendAudio(const QByteArray &opusData);

    void setSpeaking(bool speaking);

    // as the official client, SOUNDSHARE is announced once a session, never per run
    void beginSoundshare();

    bool isDaveEnabled() const;
    void requestVerificationCode(Core::Snowflake targetUserId, std::function<void(const QString &)> callback);

    // on this client's thread. set before start()
    using VideoFrameHandler = std::function<void(Core::Snowflake userId, VideoCodec codec, EncodedVideoFrame frame)>;
    void setVideoFrameHandler(VideoFrameHandler handler);

    void setVideoWants(int pixelCount, bool wanted);

    void setVideoPlan(const MediaSinkWants &wants, const QSet<quint32> &deliver);

    void requestVideoKeyframeFrom(Core::Snowflake userId);
    void requestVideoKeyframeFromEveryone();

    void withdrawVideoCodec(VideoCodec codec);
    [[nodiscard]] std::function<void(VideoCodec)> queuedCodecWithdrawal();

    void offerVideoEncoding(const QList<VideoCodec> &encodable);
    [[nodiscard]] VideoCodec videoSendCodec() const;
    // call from connected(), and again after a reconnect
    void startVideo(const LocalVideoDescription &description);
    void describeVideo(const LocalVideoDescription &description);
    void setVideoBitrate(int bitsPerSecond, int quality = FULL_QUALITY);
    void stopVideo();
    // dropped while video isnt started, or while DAVE has no key for it
    void sendVideoFrame(const EncodedVideoFrame &frame, int quality = FULL_QUALITY);

signals:
    void stateChanged(Acheron::Discord::Voice::VoiceClient::State newState);
    void connected();
    void disconnected();
    void reconnecting();

    // zero where the sender doesnt say
    void remoteVideoChanged(Acheron::Core::Snowflake userId, bool active, int maxWidth, int maxHeight, int maxFramerate);
    void remoteVideoLayersChanged(Acheron::Core::Snowflake userId, const QList<Acheron::Discord::Voice::VideoLayer> &layers);
    void videoStatsUpdated(const Acheron::Discord::Voice::VideoReceiveStats &stats);

    void videoSendCodecChanged(Acheron::Discord::Voice::VideoCodec codec);
    // before connected()
    void videoSendLayersKnown(const QList<int> &qualities);
    void videoKeyframeRequested(int quality);
    void videoReceiversChanged(int receivers, int quality);
    // `pixelCount` is zero when not said
    void videoWantsReceived(int want, int pixelCount, int quality);
    void videoSendStatsUpdated(const Acheron::Discord::Voice::VideoSendStats &stats);

    void speakingReceived(const Acheron::Discord::Voice::SpeakingData &data);
    void clientConnected(const Acheron::Discord::Voice::ClientConnectData &data);
    void clientReachedServer(Acheron::Core::Snowflake userId);
    void clientDisconnected(Acheron::Core::Snowflake userId);

    void audioReceived(quint32 ssrc, quint16 sequence, quint32 timestamp, const QByteArray &opusData);

    void privacyCodeChanged(const QString &code);

private slots:
    void onGatewayConnected();
    void onGatewayDisconnected(Acheron::Discord::Voice::VoiceCloseCode code, const QString &reason);
    void onGatewayReady(const Acheron::Discord::Voice::VoiceReady &data);
    void onSessionDescription(const Acheron::Discord::Voice::SessionDescription &desc);
    void onSessionUpdated(const Acheron::Discord::Voice::SessionUpdate &update);
    void onMediaSinkWants(const Acheron::Discord::Voice::MediaSinkWants &wants);
    void onSpeaking(const Acheron::Discord::Voice::SpeakingData &data);
    void onClientsConnect(const QStringList &userIds);
    void onClientConnect(const Acheron::Discord::Voice::ClientConnectData &data);
    void onClientDisconnect(Acheron::Core::Snowflake userId);
    void onGatewayResumed();
    void onIpDiscovered(const QString &ip, int port);
    void onIpDiscoveryFailed(const QString &error);
    void onRelayLost(const QString &error);

private:
    class VideoReceivePath;
    class VideoSendPath;

    void setState(State state);
    void sendSilence();
    void cleanupTransport();
    void ensureDaveSession(uint16_t protocolVersion);

    void onPacket(PacketPool::Handle packet);
    void receiveRtcp(PacketPool::Handle packet);
    void receiveAudio(const ParsedRtp &rtp, PacketPool::Handle packet);
    [[nodiscard]] bool openRtpPayload(const ParsedRtp &rtp, PacketBuffer &packet);
    [[nodiscard]] bool canSendMedia() const { return currentState == State::Connected && encryption && udpTransport; }
    void noteTransportSequence(std::span<const uint8_t> extensions);
    bool daveDecryptAudio(quint32 ssrc, PacketPool::Handle &packet);

    // as the official client, a codec neither encoded nor decoded any more is left out
    [[nodiscard]] QList<Codec> offeredCodecs() const;
    void sendOfferedCodecs();

    VoiceGateway *gateway = nullptr;
    UdpTransport *udpTransport = nullptr;

    ConnectionParams params;

    std::atomic<State> currentState{ State::Disconnected };

    struct UnroutedCounters
    {
        quint64 rtcp = 0;
        quint64 rtp = 0;
    };
    UnroutedCounters unrouted;

    // ready
    quint32 localSsrc = 0;
    QString serverIp;
    int serverPort = 0;
    QStringList serverModes;

    // protocol selection
    QString selectedMode;
    QByteArray sessionKey;
    bool codecsChangedSinceOffered = false;

    std::unique_ptr<VoiceEncryption> encryption;
    QTimer *keepaliveTimer = nullptr;
    uint16_t rtpSequence = 0;
    uint32_t rtpTimestamp = 0;

    std::chrono::steady_clock::time_point rtpEpoch;
    std::chrono::steady_clock::time_point lastAudioSendTime;
    bool newTalkspurt = false;
    bool soundshareAnnounced = false;

    std::unique_ptr<DaveSession> daveSession;
    std::set<std::string> connectedUserIds;
    QHash<quint32, uint64_t> ssrcToUserIdMap;

    std::unique_ptr<VideoReceivePath> receivedVideo;
    std::unique_ptr<VideoSendPath> sentVideo;

    static constexpr int KEEPALIVE_INTERVAL_MS = 10000;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Discord::Voice::VideoReceiveStats)
Q_DECLARE_METATYPE(Acheron::Discord::Voice::VideoSendStats)
Q_DECLARE_METATYPE(Acheron::Discord::Voice::VideoCodec)
