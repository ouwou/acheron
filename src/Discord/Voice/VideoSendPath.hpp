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

// The video a connection sends: the layers READY gave it to send on, the codec the server
// names, what VIDEO announces, frames end-to-end encrypted and paced out as RTP, and what the
// server says back about them.
class VoiceClient::VideoSendPath
{
public:
    explicit VideoSendPath(VoiceClient &client);
    ~VideoSendPath();

    VideoSendPath(const VideoSendPath &) = delete;
    VideoSendPath &operator=(const VideoSendPath &) = delete;

    void takeStreams(const VoiceReady &ready);
    // The transport is gone, and with it whatever was sent and announced on it. The layers
    // READY gave and the codec the server named stay.
    void reset();

    [[nodiscard]] VideoCodec codec() const { return sendCodec; }
    // The server named the codec to send with, or one this client offers changed.
    void setServerCodec(const QString &name);
    void retakeServerCodec() { setServerCodec(serverCodecName); }

    void start(const LocalVideoDescription &description);
    void describe(const LocalVideoDescription &description);
    void setBitrate(int bitrateBps, int quality);
    void stop();
    void sendFrame(const EncodedVideoFrame &frame, int quality);

    void receiveRtcp(const ReceivedRtcp &received);
    void onMediaSinkWants(const MediaSinkWants &wants);

    // The official client registers the codec for every layer's SSRC, and its retransmissions
    // need nothing: a frame is encrypted before it is cut into packets.
    void registerWithDave();
    // New end-to-end keys: every layer, or those held back for want of them, start afresh.
    void askEveryLayerForKeyframe();
    void askAwaitingLayersForKeyframe();

    static constexpr int SEND_REPORT_INTERVAL_MS = 1000;
    // An encoder asked for key frames faster than this only makes the stream worse.
    static constexpr int KEYFRAME_INTERVAL_MS = 300;
    // A delay estimate this much under the one last passed on is passed on at once.
    static constexpr double DELAY_ESTIMATE_STEP = 0.95;

private:
    // One encoding of the video this client sends, on SSRCs of its own. A stream's connection
    // has one. A voice connection's camera has the full picture, and may have a second beside
    // it.
    struct Layer
    {
        int quality = FULL_QUALITY;
        quint32 ssrc = 0;
        quint32 rtxSsrc = 0;
        std::unique_ptr<VideoSendStream> sender;
        // What the layer is being encoded at, once its sender has said. Until then, the most
        // that was announced for it stands in.
        std::optional<int> bitrateBps;
        std::optional<int> receivers;
        // Something of the layer did not go out (there was no end-to-end key for it yet, or
        // the codec changed under it), so what follows builds on pictures nobody was sent
        // and is held back until a key frame comes.
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
    // Points every layer's sender at the codec in use, making them first if need be. False when
    // video cannot be sent with that codec.
    bool prepareSenders();
    void paceByBitrate();
    void announce();
    void announceOff();
    bool daveEncrypt(std::span<const uint8_t> frame, quint32 ssrc, std::vector<uint8_t> &encrypted);
    void startPacing();
    void onPaceTick();
    void onSendReportTick();
    void noteReceivers(Layer &layer, int receivers);
    void askLayersForKeyframe(const std::function<bool(const Layer &)> &which);
    void askForKeyframe(Layer &layer);
    [[nodiscard]] qint64 nowUs() const { return clock.nsecsElapsed() / 1000; }

    VoiceClient &client;
    QElapsedTimer clock;

    // The video layers READY gave this client to send on, as it described them and as they
    // are sent.
    QList<StreamDescription> streams;
    std::vector<Layer> layers;
    VideoCodec sendCodec = VideoCodec::Unknown;
    // The codec the server last named, which is taken up once this client offers to encode it.
    QString serverCodecName;

    // Set while video is started.
    std::optional<LocalVideoDescription> started;
    // The layers as VIDEO last described them, which is how they are named again, inactive,
    // when video goes off.
    QList<StreamDescription> announcedStreams;
    QTimer *paceTimer = nullptr;
    QTimer *sendReportTimer = nullptr;
    QTimer *keyframeTimer = nullptr;
    quint64 keyframeRequests = 0;
    quint64 receiverEstimateBps = 0;
    // Numbers every video packet of the connection, and judges the way from the server's
    // reports on them.
    SendSideEstimator estimator;
    quint64 reportedDelayEstimateBps = 0;
    qint64 sendReportedUs = 0;
    // Kept between frames, which it is as large as several of.
    std::vector<uint8_t> encryptedFrame;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
