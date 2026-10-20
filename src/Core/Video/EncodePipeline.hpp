#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <memory>

#include "Discord/Voice/VideoFrame.hpp"

namespace Acheron {
namespace Core {
namespace Video {

using EncodedFramePointer = std::shared_ptr<const Discord::Voice::EncodedVideoFrame>;

struct PipelineStats
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

// What a screen's and a camera's pictures are encoded by. Each pipeline captures and encodes on
// a thread of its own and emits its signals from there, so connect to them queued or from an
// object on another thread. Connected in the pipeline's own context, what it had on its way
// when it is let go of goes with it.
//
// Encoded frames are whole access units as the RTP sender wants them, with a 90 kHz timestamp
// taken from when the picture was captured: Annex B for H264 and H265, every key frame led by
// its parameter sets; OBUs with their sizes for AV1, every key frame holding a sequence header;
// a bare frame for VP8. A pipeline may encode its pictures more than once, each encoding a
// layer of its own: layer 0 is the one its settings describe.
class EncodePipeline : public QObject
{
    Q_OBJECT
public:
    ~EncodePipeline() override;

signals:
    // Again whenever the size or frame rate a layer is encoded at changes.
    void started(const QSize &encodedSize, int framerate, int layer);
    void frameEncoded(const Acheron::Core::Video::EncodedFramePointer &frame, int layer);
    void previewReady(const QImage &preview);
    // Nothing more will come: the source is gone, or no encoder could be opened.
    void failed(const QString &reason);
    // About once a second.
    void statsUpdated(const Acheron::Core::Video::PipelineStats &stats, int layer);

protected:
    explicit EncodePipeline(QObject *parent = nullptr);
};

} // namespace Video
} // namespace Core
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Core::Video::EncodedFramePointer)
Q_DECLARE_METATYPE(Acheron::Core::Video::PipelineStats)
