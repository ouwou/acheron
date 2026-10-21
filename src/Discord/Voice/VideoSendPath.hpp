#pragma once

#include <QElapsedTimer>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "SendSideEstimator.hpp"
#include "VideoSendStream.hpp"
#include "VoiceClient.hpp"

class QTimer;

namespace Acheron {
namespace Discord {
namespace Voice {

class VoiceClient::VideoSendPath
{
public:
    explicit VideoSendPath(VoiceClient &client);
    ~VideoSendPath();

    VideoSendPath(const VideoSendPath &) = delete;
    VideoSendPath &operator=(const VideoSendPath &) = delete;

    void takeStreams(const VoiceReady &ready);
    void reset();

    [[nodiscard]] VideoCodec codec() const { return sendCodec; }
    void setServerCodec(const QString &name);
    void retakeServerCodec() { setServerCodec(serverCodecName); }

    void start(const LocalVideoDescription &description);
    void describe(const LocalVideoDescription &description);
    void setBitrate(int bitrateBps, int quality);
    void stop();
    void sendFrame(const EncodedVideoFrame &frame, int quality);

    void receiveRtcp(const ReceivedRtcp &received);
    void onMediaSinkWants(const MediaSinkWants &wants);

    // as the official client: every layer's SSRC, not the RTX ones, since frames are encrypted before
    // packetizing
    void registerWithDave();
    void askEveryLayerForKeyframe();
    void askAwaitingLayersForKeyframe();

    static constexpr int SEND_REPORT_INTERVAL_MS = 1000;
    // asking faster only makes the stream worse
    static constexpr int KEYFRAME_INTERVAL_MS = 300;
    static constexpr double DELAY_ESTIMATE_STEP = 0.95;

private:
    struct Layer
    {
        int quality = FULL_QUALITY;
        quint32 ssrc = 0;
        quint32 rtxSsrc = 0;
        std::unique_ptr<VideoSendStream> sender;
        std::optional<int> bitrateBps;
        std::optional<int> receivers;
        bool awaitsKeyframe = false;
        std::optional<qint64> keyframeAskedMs;
        bool keyframeAskWaiting = false;
        quint64 bytesAtSendReport = 0;
    };

    [[nodiscard]] Layer *layerOfQuality(int quality);
    [[nodiscard]] Layer *layerOf(quint32 ssrc);
    [[nodiscard]] const Layer *fullLayer() const;
    [[nodiscard]] int announcedMaxBitrate(const Layer &layer) const;
    [[nodiscard]] quint32 videoSsrc() const;
    [[nodiscard]] bool anyWaiting() const;
    void emitLayersKnown();
    bool prepareSenders();
    void paceByBitrate();
    void announce();
    void announceOff();
    bool daveEncrypt(std::span<const uint8_t> frame, quint32 ssrc, std::vector<uint8_t> &encrypted);
    void startPacing();
    void onPaceTick();
    void onSendReportTick();
    void sendSenderReports(qint64 now);
    void emitSendStats(qint64 now);
    [[nodiscard]] const Codec *offeredForEncoding(VideoCodec codec) const;
    void noteReceivers(Layer &layer, int receivers);
    void askLayersForKeyframe(const std::function<bool(const Layer &)> &which);
    void askForKeyframe(Layer &layer);
    [[nodiscard]] qint64 nowUs() const { return clock.nsecsElapsed() / 1000; }

    VoiceClient &client;
    QElapsedTimer clock;

    QList<StreamDescription> streams;
    std::vector<Layer> layers;
    VideoCodec sendCodec = VideoCodec::Unknown;
    QString serverCodecName;

    std::optional<LocalVideoDescription> started;
    QList<StreamDescription> announcedStreams;
    std::unique_ptr<QTimer> paceTimer;
    std::unique_ptr<QTimer> sendReportTimer;
    std::unique_ptr<QTimer> keyframeTimer;
    quint64 keyframeRequests = 0;
    std::optional<quint64> receiverEstimateBps;
    SendSideEstimator estimator;
    std::optional<qint64> reportedDelayEstimateBps;
    qint64 sendReportedUs = 0;
    std::vector<uint8_t> encryptedFrame;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
