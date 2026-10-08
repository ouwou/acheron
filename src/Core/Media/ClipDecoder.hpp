#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include <memory>

namespace Acheron {
namespace Core {
namespace Media {

class ClipDecoder
{
public:
    virtual ~ClipDecoder() = default;

    [[nodiscard]] static std::unique_ptr<ClipDecoder> open(const QString &path);

    [[nodiscard]] virtual QSize nativeSize() const = 0;
    virtual bool nextFrameOfLoop(QImage &frame, qint64 &startMs, const QSize &frameSize) = 0;
    [[nodiscard]] virtual qint64 finishedLoopMs() const = 0;
    virtual bool rewind() = 0;
};

} // namespace Media
} // namespace Core
} // namespace Acheron
