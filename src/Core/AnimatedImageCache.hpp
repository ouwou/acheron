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

namespace Apng {
class Reader;
} // namespace Apng

class AnimatedImageCache : public QObject
{
    Q_OBJECT
public:
    struct MiBRange
    {
        int min;
        int fallback;
        int max;
    };

    static constexpr MiBRange CacheLimit{ 64, 96, 512 };
    static constexpr MiBRange AnimationLimit{ 8, 24, 64 };

    explicit AnimatedImageCache(ImageManager *imageManager, QObject *parent = nullptr);
    ~AnimatedImageCache() override;

    void setCacheLimitMiB(int mib);
    // takes effect for animations decoded from now on
    void setAnimationLimitMiB(int mib);

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

    struct DecodeTarget
    {
        QSize physicalSize;
        qsizetype byteBudget;
    };

    void onRawDownloadReady(const QUrl &url, const QSize &size);
    void decode(const ImageRequestKey &key, const QString &path);
    void publish(const ImageRequestKey &key, const Decoded &decoded, qreal dpr);
    static Decoded decodeFrames(const QString &path, const DecodeTarget &target);
    static Decoded decodeApng(Apng::Reader &apng, const DecodeTarget &target);
    static Decoded decodeLottie(const QByteArray &data, const DecodeTarget &target);
    static Decoded decodeWithImageReader(const QByteArray &data, const DecodeTarget &target);

    ImageManager *imageManager;
    QThreadPool decodePool;
    int animationLimitMiB = AnimationLimit.fallback;
    int newestFirstPriority = 0;
    QSet<ImageRequestKey> wanted;
    QSet<ImageRequestKey> decoding;
    QCache<ImageRequestKey, AnimatedFramesPtr> cache;
};

} // namespace Core
} // namespace Acheron
