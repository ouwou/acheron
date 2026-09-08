#include "AnimatedImageCache.hpp"

#include <QGuiApplication>
#include <QImageReader>

#include <algorithm>

#include "Logging.hpp"

namespace Acheron {
namespace Core {

namespace {

constexpr int MaxFrames = 200;
constexpr qsizetype MaxBytesPerAnimation = 8 * 1024 * 1024;
constexpr int MaxCacheKiB = 48 * 1024;

int normalizedDelay(int delayMs)
{
    if (delayMs <= 10)
        return 100;
    return std::max(delayMs, 20);
}

} // namespace

int AnimatedFrames::frameAt(qint64 loopPhaseMs) const
{
    if (isStatic())
        return 0;
    auto it = std::upper_bound(frameEndMs.begin(), frameEndMs.end(), int(loopPhaseMs));
    return int(std::min<qsizetype>(it - frameEndMs.begin(), frames.size() - 1));
}

AnimatedImageCache::AnimatedImageCache(ImageManager *imageManager, QObject *parent)
    : QObject(parent), imageManager(imageManager)
{
    decodePool.setMaxThreadCount(1);
    cache.setMaxCost(MaxCacheKiB);
    connect(imageManager, &ImageManager::imageFetched, this,
            [this](const QUrl &url, const QSize &size, const QPixmap &) { onImageFetched(url, size); });
}

AnimatedImageCache::~AnimatedImageCache()
{
    decodePool.clear();
    decodePool.waitForDone();
}

AnimatedFramesPtr AnimatedImageCache::get(const QUrl &url, const QSize &logicalSize, Snowflake accountId)
{
    const ImageRequestKey key{ url, logicalSize };
    if (const AnimatedFramesPtr *cached = cache.object(key))
        return *cached;
    if (decoding.contains(key))
        return nullptr;

    wanted.insert(key);
    imageManager->get(url, logicalSize, accountId);
    const QString path = imageManager->rawDownloadPath(url, logicalSize);
    if (!path.isEmpty())
        decode(key, path);
    return nullptr;
}

void AnimatedImageCache::onImageFetched(const QUrl &url, const QSize &size)
{
    const ImageRequestKey key{ url, size };
    if (!wanted.contains(key) || decoding.contains(key))
        return;
    const QString path = imageManager->rawDownloadPath(url, size);
    if (path.isEmpty())
        wanted.remove(key);
    else
        decode(key, path);
}

void AnimatedImageCache::decode(const ImageRequestKey &key, const QString &path)
{
    decoding.insert(key);

    const qreal dpr = qGuiApp->devicePixelRatio();
    const QSize physicalSize = key.size * dpr;

    // newest first
    // clang-format off
    decodePool.start([this, key, path, physicalSize, dpr]() {
        const Decoded decoded = decodeFrames(path, physicalSize);
        QMetaObject::invokeMethod(this, [this, key, decoded, dpr]() { publish(key, decoded, dpr); }, Qt::QueuedConnection);
    }, ++newestFirstPriority);
    // clang-format on
}

AnimatedImageCache::Decoded AnimatedImageCache::decodeFrames(const QString &path, const QSize &physicalSize)
{
    Decoded out;

    QImageReader reader(path);
    if (!reader.canRead() || !reader.supportsAnimation())
        return out;

    qsizetype bytes = 0;
    for (;;) {
        QImage frame = reader.read();
        if (frame.isNull())
            break;

        const int delay = reader.nextImageDelay();

        if (frame.size() != physicalSize)
            frame = frame.scaled(physicalSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (frame.format() != QImage::Format_ARGB32_Premultiplied)
            frame = frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        bytes += frame.sizeInBytes();
        if (bytes > MaxBytesPerAnimation || out.frames.size() >= MaxFrames) {
            qCDebug(LogCore) << "Animated image exceeds frame budget, rendering static";
            return Decoded{};
        }

        out.frames.append(frame);
        out.delaysMs.append(normalizedDelay(delay));
    }

    if (out.frames.size() <= 1)
        return Decoded{};
    return out;
}

void AnimatedImageCache::publish(const ImageRequestKey &key, const Decoded &decoded, qreal dpr)
{
    auto frames = std::make_shared<AnimatedFrames>();
    qsizetype bytes = 0;
    int elapsed = 0;
    for (qsizetype i = 0; i < decoded.frames.size(); ++i) {
        QPixmap pixmap = QPixmap::fromImage(decoded.frames[i]);
        pixmap.setDevicePixelRatio(dpr);
        frames->frames.append(pixmap);
        bytes += decoded.frames[i].sizeInBytes();
        elapsed += decoded.delaysMs[i];
        frames->frameEndMs.append(elapsed);
    }

    const AnimatedFramesPtr published = std::move(frames);
    cache.insert(key, new AnimatedFramesPtr(published), int(std::max<qsizetype>(1, bytes / 1024)));
    decoding.remove(key);
    wanted.remove(key);
    emit framesReady(key.url, key.size, published);
}

} // namespace Core
} // namespace Acheron
