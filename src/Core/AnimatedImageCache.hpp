#pragma once

#include <QCache>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QSize>
#include <QThreadPool>
#include <QUrl>

#include <memory>

#include "ImageManager.hpp"
#include "Snowflake.hpp"

namespace Acheron {
namespace Core {

struct AnimatedFrames
{
    QList<QPixmap> frames;
    QList<int> frameEndMs;

    [[nodiscard]] bool isStatic() const { return frames.size() <= 1; }
    [[nodiscard]] int loopMs() const { return frameEndMs.isEmpty() ? 0 : frameEndMs.last(); }
    [[nodiscard]] int frameAt(qint64 loopPhaseMs) const;
};

using AnimatedFramesPtr = std::shared_ptr<const AnimatedFrames>;

class AnimatedImageCache : public QObject
{
    Q_OBJECT
public:
    explicit AnimatedImageCache(ImageManager *imageManager, QObject *parent = nullptr);
    ~AnimatedImageCache() override;

    // null while fetching or decoding
    AnimatedFramesPtr get(const QUrl &url, const QSize &logicalSize, Snowflake accountId);

signals:
    void framesReady(const QUrl &url, const QSize &logicalSize, const AnimatedFramesPtr &frames);

private:
    struct Decoded
    {
        QList<QImage> frames;
        QList<int> delaysMs;
    };

    void onImageFetched(const QUrl &url, const QSize &size);
    void decode(const ImageRequestKey &key, const QString &path);
    void publish(const ImageRequestKey &key, const Decoded &decoded, qreal dpr);
    static Decoded decodeFrames(const QString &path, const QSize &physicalSize);

    ImageManager *imageManager;
    QThreadPool decodePool;
    int newestFirstPriority = 0;
    QSet<ImageRequestKey> wanted;
    QSet<ImageRequestKey> decoding;
    QCache<ImageRequestKey, AnimatedFramesPtr> cache;
};

} // namespace Core
} // namespace Acheron
