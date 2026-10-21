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

class VoiceClient::VideoReceivePath
{
public:
    explicit VideoReceivePath(VoiceClient &client);
    ~VideoReceivePath();

    VideoReceivePath(const VideoReceivePath &) = delete;
    VideoReceivePath &operator=(const VideoReceivePath &) = delete;

    void setFrameHandler(VideoFrameHandler handler) { frameHandler = std::move(handler); }

    bool receive(const ParsedRtp &rtp, PacketPool::Handle &packet);
    void noteTransportSequence(std::span<const uint8_t> extensions);
    void receiveRtcp(const ReceivedRtcp &received);

    void onConnected();
    void onClientConnect(const ClientConnectData &data);
    void onClientDisconnect(Core::Snowflake userId);
    void onDaveStateChanged();
    void reset();

    void setWants(int pixelCount, bool wanted);
    void setPlan(const MediaSinkWants &wants, const QSet<quint32> &deliver);
    void requestKeyframeFrom(Core::Snowflake userId);
    void requestKeyframeFromEveryone();

    static constexpr int TICK_MS = 20;
    static constexpr int REPORT_INTERVAL_MS = 1000;
    static constexpr int TRANSPORT_FEEDBACK_INTERVAL_MS = 100;
    static constexpr int MAX_TRANSPORT_FEEDBACKS_PER_PACKET = 4;

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

    struct Session
    {
        std::unordered_map<quint32, RemoteVideo> remoteVideos;
        QHash<quint32, quint32> rtxToPrimary;
        QHash<Core::Snowflake, QList<VideoLayer>> remoteLayers;

        MediaSinkWants plannedWants;
        QSet<quint32> deliveredSsrcs;
        bool explicitPlan = false;
        int wantedPixelCount = 0;
        bool wanted = true;
        QByteArray sentWants;

        bool droppedAwaitingDaveKeys = false;
        quint64 decryptFailures = 0;
        qint64 reportedMs = 0;

        // without it the server holds every stream at a low bitrate
        TransportFeedback transportFeedback;
        qint64 transportReportedMs = 0;
        quint32 transportMediaSsrc = 0;

        QHash<quint32, HeardSenderReport> senderReports;
        std::optional<InboundEstimate> serverEstimate;
    };

    void receiveVideo(const ParsedRtp &rtp, PacketPool::Handle packet, VideoCodec codec, VideoReceiver::Arrival arrival);
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
    QHash<int, VideoCodec> payloadTypes;
    QHash<int, VideoCodec> rtxPayloadTypes;
    QElapsedTimer clock;
    std::unique_ptr<QTimer> tick;

    Session state;
    std::vector<uint8_t> decrypted;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
