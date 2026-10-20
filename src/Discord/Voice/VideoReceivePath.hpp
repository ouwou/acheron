#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QSet>

#include <memory>
#include <unordered_map>

#include "TransportFeedback.hpp"
#include "VideoReceiver.hpp"
#include "VoiceClient.hpp"

class QTimer;

namespace Acheron {
namespace Discord {
namespace Voice {

// The video a connection receives: which of it is wanted and from whom, its packets put back
// together into frames and handed on, and the feedback the server is given about it, which is
// on every packet the server numbered, video or not.
class VoiceClient::VideoReceivePath
{
public:
    explicit VideoReceivePath(VoiceClient &client);
    ~VideoReceivePath();

    VideoReceivePath(const VideoReceivePath &) = delete;
    VideoReceivePath &operator=(const VideoReceivePath &) = delete;

    void setFrameHandler(VideoFrameHandler handler) { frameHandler = std::move(handler); }

    // False, leaving `packet` alone, when its payload type is not one offered for video.
    bool receive(const ParsedRtp &rtp, PacketPool::Handle &packet);
    // Takes the packet's place in the server's numbering of everything it sends this way,
    // from its decrypted header extensions, for the feedback that goes back.
    void noteTransportSequence(std::span<const uint8_t> extensions);
    void receiveRtcp(const ReceivedRtcp &received);

    void onConnected();
    void onClientConnect(const ClientConnectData &data);
    void onClientDisconnect(Core::Snowflake userId);
    // End-to-end keys came or went; frames dropped for want of them are asked for again.
    void onDaveStateChanged();
    // The transport is gone, and with it whatever was received and asked for on it.
    void reset();

    void setWants(int pixelCount, bool wanted);
    void setPlan(const MediaSinkWants &wants, const QSet<quint32> &deliver);
    void requestKeyframeFrom(Core::Snowflake userId);
    void requestKeyframeFromEveryone();

    static constexpr int TICK_MS = 20;
    static constexpr int REPORT_INTERVAL_MS = 1000;
    static constexpr int TRANSPORT_FEEDBACK_INTERVAL_MS = 100;

private:
    struct RemoteVideo
    {
        Core::Snowflake userId;
        quint32 rtxSsrc = 0;
        std::unique_ptr<VideoReceiver> receiver;
    };

    struct HeardSenderReport
    {
        quint32 compactNtp = 0;
        qint64 arrivalUs = 0;
    };

    // All of it is for one transport.
    struct Session
    {
        // Remote video by its SSRC, and each RTX SSRC's primary.
        std::unordered_map<quint32, RemoteVideo> remoteVideos;
        QHash<quint32, quint32> rtxToPrimary;
        QHash<Core::Snowflake, QList<VideoLayer>> remoteLayers;

        // What the server is asked for, and the SSRCs whose video is received and handed on.
        MediaSinkWants plannedWants;
        QSet<quint32> deliveredSsrcs;
        bool explicitPlan = false;
        int wantedPixelCount = 0;
        bool wanted = true;
        QByteArray sentWants;

        bool droppedAwaitingDaveKeys = false;
        quint64 decryptFailures = 0;
        qint64 reportedMs = 0;

        // Which packets arrived and when, for the server's estimate of what this connection
        // can carry. Without it the server holds every stream at a low bitrate.
        TransportFeedback transportFeedback;
        qint64 transportReportedMs = 0;
        quint32 transportMediaSsrc = 0;

        QHash<quint32, HeardSenderReport> senderReports;
        quint64 serverEstimateBps = 0;
        double serverLossFraction = 0;
    };

    void receiveVideo(const ParsedRtp &rtp, PacketPool::Handle packet, VideoCodec codec, bool retransmission);
    void deliverFrames(quint32 ssrc, RemoteVideo &video);
    bool daveDecrypt(quint32 ssrc, std::vector<uint8_t> &frame);
    void receiveAfresh(quint32 ssrc);
    void removeRemoteVideo(Core::Snowflake userId);
    void planSingleSenders();
    void applyPlan();
    void sendWants();
    void onTick();
    [[nodiscard]] qint64 nowUs() const { return clock.nsecsElapsed() / 1000; }

    VoiceClient &client;
    VideoFrameHandler frameHandler;
    // The codec behind each payload type offered for decoding, for video and for its RTX.
    QHash<int, VideoCodec> payloadTypes;
    QHash<int, VideoCodec> rtxPayloadTypes;
    QElapsedTimer clock;
    QTimer *tick = nullptr;

    Session state;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
