#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "PacketBuffer.hpp"
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

// What one connection has received of remote video, summed over its senders.
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
    // Share of packets lost since the previous report, 0 to 1.
    double lossFraction = 0;
    // What the server says the way to this client carries, and loses. Zero until it has said.
    quint64 serverEstimateBps = 0;
    double serverLossFraction = 0;
};

// What this client announces of the video it sends, in VIDEO (opcode 12).
struct LocalVideoDescription
{
    // The largest picture that will be sent. Zero by zero means the source's own size, whatever
    // that turns out to be.
    int maxWidth = 0;
    int maxHeight = 0;
    int maxFramerate = 30;
    // Bits per second.
    int maxBitrate = 0;
    // The most any layer beside the full-quality one is sent at. Zero gives them the same.
    int secondLayerMaxBitrate = 0;
};

// What one connection has sent of its own video, over all the layers it is sent in, and what
// the server says became of it.
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
    // Sent over the last second, retransmissions included.
    quint64 bitrateBps = 0;
    // From the server's reception reports on the video: the share of packets lost since its
    // previous report, 0 to 1, and the round trip, each the worst of the layers'. Negative
    // until it has reported.
    double lossFraction = -1;
    int roundTripMs = -1;
    // How many are receiving the full-quality layer, as the server counts them. Negative
    // until it has said.
    int receivers = -1;
    // The most the receiving side says the way from here carries (REMB). Zero until it has
    // said.
    quint64 receiverEstimateBps = 0;
    // The most the way carries going by when each packet reached the server, and what the
    // server last acknowledged receiving. The first is zero until the way has once been
    // overused, the second until the server has reported for half a second.
    quint64 delayEstimateBps = 0;
    quint64 acknowledgedBps = 0;
};

// What a voice connection's IDENTIFY declares for the account's camera: the full picture, and
// with `withSecondLayer` a second encoding beside it, `rid` "50" at quality 50, as the official
// client declares while its simulcast switch is on. Viewers choose between the two by SSRC,
// and one who sees a single camera takes the second.
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

    // On a stream's connection, whose audio is the sound its owner shares: a run of that sound
    // starts, so the next frame carries the marker and a timestamp taken from the clock again.
    // The server is told of the sound once a session, with the SOUNDSHARE flag, as the official
    // client tells it: no later run is announced, and neither is the end of one.
    void beginSoundshare();

    bool isDaveEnabled() const;
    void requestVerificationCode(Core::Snowflake targetUserId, std::function<void(const QString &)> callback);

    // Receives every complete, decrypted frame of remote video, on this client's thread. Set it
    // before start().
    using VideoFrameHandler = std::function<void(Core::Snowflake userId, VideoCodec codec, EncodedVideoFrame frame)>;
    void setVideoFrameHandler(VideoFrameHandler handler);

    // How much of the remote video is wanted: the size it is being shown at, in pixels, or
    // nothing at all while it is not shown. This is all a stream's viewer has to say.
    void setVideoWants(int pixelCount, bool wanted);

    // The full form, for a call with several senders: what to tell the server, and which
    // SSRCs to receive and hand on. Everything else that arrives is discarded.
    void setVideoPlan(const MediaSinkWants &wants, const QSet<quint32> &deliver);

    // A frame handed out could not be decoded; ask its sender to start over.
    void requestVideoKeyframeFrom(Core::Snowflake userId);
    void requestVideoKeyframeFromEveryone();

    // No decoder here can handle the codec after all: tell the server, so that it has senders
    // use another.
    void withdrawVideoCodec(VideoCodec codec);

    // Sending video. Like everything else here, these are called on this client's thread.
    //
    // The codecs video can be sent with are these and no others from now on: encoders turned
    // out to exist after the connection was made, or one is not to be used for what is being
    // sent. The server is told in SELECT_PROTOCOL when that is still to come, and otherwise in
    // a session update of this client's own, which it answers with the codec to use. Until it
    // has, a codec that was withdrawn while in use leaves nothing to send with.
    void offerVideoEncoding(const QList<VideoCodec> &encodable);
    // The codec the server has this client send with: one of those offered with `encode` set.
    // Unknown until the session is described.
    [[nodiscard]] VideoCodec videoSendCodec() const;
    // Announces the video and starts accepting frames. Has no effect before the connection is
    // up; call it from connected(), and again after a reconnect.
    void startVideo(const LocalVideoDescription &description);
    // The limits changed while sending: announces them again.
    void describeVideo(const LocalVideoDescription &description);
    // What a layer of the video is being encoded at just now, in bits per second. Its packets
    // are paced by it; until it is given, by the most that was announced. A layer is named by
    // its quality: FULL_QUALITY, or what videoSendLayersKnown() listed beside it.
    void setVideoBitrate(int bitsPerSecond, int quality = FULL_QUALITY);
    // Announces that the video is off and forgets what was queued.
    void stopVideo();
    // One whole encoded frame in videoSendCodec(): Annex B for H264 and H265, with the parameter
    // sets in front of every key frame, and for AV1 one temporal unit of OBUs that each have
    // their size field, a key frame's opening with its sequence header. `keyframe` is taken at
    // its word. `rtpTimestamp` is at 90 kHz from any origin. Frames are dropped while video is
    // not started, and while end-to-end encryption has no key to send them under. Each layer
    // is a stream of its own, with its own key frames.
    void sendVideoFrame(const EncodedVideoFrame &frame, int quality = FULL_QUALITY);

    static constexpr int FULL_QUALITY = 100;

signals:
    void stateChanged(Acheron::Discord::Voice::VoiceClient::State newState);
    void connected();
    void disconnected();
    // The gateway socket dropped and is being reopened.
    void reconnecting();

    // A user's video started, changed or (inactive) stopped. The size and rate are the most the
    // sender says it will send, and are zero when it does not say.
    void remoteVideoChanged(Acheron::Core::Snowflake userId, bool active, int maxWidth, int maxHeight, int maxFramerate);
    // The encodings a user's video now comes in; none when it has stopped.
    void remoteVideoLayersChanged(Acheron::Core::Snowflake userId, const QList<Acheron::Discord::Voice::VideoLayer> &layers);
    void videoStatsUpdated(const Acheron::Discord::Voice::VideoReceiveStats &stats);

    void videoSendCodecChanged(Acheron::Discord::Voice::VideoCodec codec);
    // The layers the server gave this client to send video in, by their quality. Said when
    // the connection is made, before connected().
    void videoSendLayersKnown(const QList<int> &qualities);
    // A viewer of a layer cannot go on without a key frame, or a new one has arrived.
    void videoKeyframeRequested(int quality);
    // The server's count of who is receiving a layer changed. At zero there is no point in
    // encoding it.
    void videoReceiversChanged(int receivers, int quality);
    // What the server relays of the viewers' wishes for a layer being sent: how much of it
    // is wanted, 0 to 100, and the largest size any of them shows it at, in pixels (zero when
    // not said).
    void videoWantsReceived(int want, int pixelCount, int quality);
    // Once a second while video is being sent.
    void videoSendStatsUpdated(const Acheron::Discord::Voice::VideoSendStats &stats);

    void speakingReceived(const Acheron::Discord::Voice::SpeakingData &data);
    void clientConnected(const Acheron::Discord::Voice::ClientConnectData &data);
    // The voice server says this user is connected to it, whatever the main gateway has said
    // of their voice state.
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
    void noteTransportSequence(std::span<const uint8_t> extensions);
    // Replaces the packet with its end-to-end decrypted contents.
    bool daveDecryptAudio(quint32 ssrc, PacketPool::Handle &packet);

    // A video codec that is neither encoded nor decoded any more is left out, as the official
    // client leaves it out.
    [[nodiscard]] QList<Codec> offeredCodecs() const;
    void sendOfferedCodecs();

    VoiceGateway *gateway = nullptr;
    UdpTransport *udpTransport = nullptr;

    ConnectionParams params;

    std::atomic<State> currentState{ State::Disconnected };

    // Packets this connection receives but has no consumer for yet.
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
    // SELECT_PROTOCOL went out before the codecs offered for encoding were all known.
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

    // Present on a connection that declared video, and only there.
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
