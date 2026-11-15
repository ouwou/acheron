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

// emits from its own thread. connected with the pipeline itself as context, a signal still queued is
// dropped with the pipeline
class EncodePipeline : public QObject
{
    Q_OBJECT
public:
    ~EncodePipeline() override;

signals:
    void started(const QSize &encodedSize, int framerate, int layer);
    void frameEncoded(const Acheron::Core::Video::EncodedFramePointer &frame, int layer);
    void previewReady(const QImage &preview);
    void failed(const QString &reason);
    void statsUpdated(const Acheron::Core::Video::PipelineStats &stats, int layer);

protected:
    explicit EncodePipeline(QObject *parent = nullptr);
};

} // namespace Video
} // namespace Core
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Core::Video::EncodedFramePointer)
Q_DECLARE_METATYPE(Acheron::Core::Video::PipelineStats)
