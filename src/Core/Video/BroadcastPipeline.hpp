#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>

#include <memory>

#include "Discord/Voice/VideoFrame.hpp"

namespace Acheron {
namespace Core {
namespace Video {

struct BroadcastSettings
{
    QString sourceId;
    // The tallest picture to send; a taller source is scaled down keeping its shape. Zero
    // sends the source at its own size.
    int maxHeight = 720;
    int framerate = 30;
    Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown;
    // Bits per second.
    int bitrate = 0;
    int maxBitrate = 0;
    // From the server; zero means key frames are made only when asked for.
    int keyframeIntervalMs = 0;
};

struct BroadcastStats
{
    QString encoderName;
    bool hardware = false;
    QSize encodedSize;
    double capturedFps = 0;
    double encodedFps = 0;
    double averageEncodeMs = 0;
    quint64 bitrateBps = 0;
    quint64 framesEncoded = 0;
    quint64 framesDropped = 0;
};

using EncodedFramePointer = std::shared_ptr<const Discord::Voice::EncodedVideoFrame>;

// Captures a source, scales it and encodes it, on a thread of its own. Its signals are
// emitted from that thread, so connect to them queued or from an object on another thread.
//
// Encoded frames are whole access units as the RTP sender wants them, with a 90 kHz timestamp
// taken from when the picture was captured: Annex B for H264 and H265, every key frame led by
// its parameter sets; OBUs with their sizes for AV1, every key frame holding a sequence
// header; a bare frame for VP8.
//
// A source whose size the codec's encoder cannot code acceptably (Media::paddingIsAcceptable)
// is captured and reported and not encoded: started() says the size, and nothing follows
// until the pipeline is started again with another codec or the source changes its size.
class BroadcastPipeline : public QObject
{
    Q_OBJECT
public:
    explicit BroadcastPipeline(QObject *parent = nullptr);
    ~BroadcastPipeline() override;

    // All of these may be called from any thread and return at once.
    void start(const BroadcastSettings &settings);
    void stop();
    // The first frame encoded after this is a key frame.
    void requestKeyframe();
    // Encoders that cannot follow a new rate while running are reopened, which costs a key
    // frame, so callers should move this in steps and not continuously.
    void setBitrate(int bitrate);
    // While nobody is watching: nothing is encoded, and the source is read only as often as
    // the previews need it. Sending again starts with a key frame.
    void setEncodingSuspended(bool suspended);

signals:
    void started(const QSize &encodedSize, int framerate);
    void frameEncoded(const Acheron::Core::Video::EncodedFramePointer &frame);
    // About twice a second, and small: for the sharer's own tile and for the stream's thumbnail.
    void previewReady(const QImage &preview);
    // The source stopped delivering pictures (true) or resumed (false).
    void captureStalled(bool stalled);
    // The window being shared was closed. Nothing more will come, and nothing went wrong.
    void sourceClosed();
    // Nothing more will come: the source is gone, or no encoder could be opened.
    void failed(const QString &reason);
    void statsUpdated(const Acheron::Core::Video::BroadcastStats &stats);

private:
    class Worker;
    std::unique_ptr<Worker> worker;
};

} // namespace Video
} // namespace Core
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Core::Video::EncodedFramePointer)
Q_DECLARE_METATYPE(Acheron::Core::Video::BroadcastStats)
