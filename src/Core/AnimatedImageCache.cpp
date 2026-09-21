#include "AnimatedImageCache.hpp"

#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>

#include <algorithm>
#include <cmath>
#include <optional>

#include "ApngDecoder.hpp"
#include "Logging.hpp"
#include "LottieDecoder.hpp"

namespace Acheron {
namespace Core {

namespace {

constexpr int MaxFrames = 300;
constexpr int KiBPerMiB = 1024;
constexpr qsizetype BytesPerMiB = 1024 * 1024;
constexpr double MinBudgetScale = 0.5;

int normalizedDelay(int delayMs)
{
    if (delayMs <= 10)
        return 100;
    return std::max(delayMs, 20);
}

std::optional<QSize> frameSizeWithinBudget(const QSize &wanted, int frameCount, qsizetype byteBudget)
{
    if (frameCount > MaxFrames || wanted.isEmpty())
        return std::nullopt;

    const double wantedBytes = double(frameCount) * wanted.width() * wanted.height() * 4;
    if (wantedBytes <= double(byteBudget))
        return wanted;

    const double scale = std::sqrt(double(byteBudget) / wantedBytes);
    if (scale < MinBudgetScale)
        return std::nullopt;
    return QSize(std::max(1, int(wanted.width() * scale)), std::max(1, int(wanted.height() * scale)));
}

QImage scaledFrame(const QImage &source, const QSize &size)
{
    QImage frame = source.size() == size ? source : source.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (frame.format() != QImage::Format_ARGB32_Premultiplied)
        frame = frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return frame;
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
    setCacheLimitMiB(CacheLimit.fallback);
    connect(imageManager, &ImageManager::rawDownloadReady, this, &AnimatedImageCache::onRawDownloadReady);
}

static_assert(AnimatedImageCache::AnimationLimit.max <= AnimatedImageCache::CacheLimit.min,
              "QCache deletes an entry that costs more than the whole cache, and it would be decoded again on every paint");

void AnimatedImageCache::setCacheLimitMiB(int mib)
{
    cache.setMaxCost(std::clamp(mib, CacheLimit.min, CacheLimit.max) * KiBPerMiB);
}

void AnimatedImageCache::setAnimationLimitMiB(int mib)
{
    animationLimitMiB = std::clamp(mib, AnimationLimit.min, AnimationLimit.max);
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
    const QString path = imageManager->rawDownloadPath(url, logicalSize);
    if (path.isEmpty())
        imageManager->downloadToCache(url, logicalSize, accountId);
    else
        decode(key, path);
    return nullptr;
}

void AnimatedImageCache::onRawDownloadReady(const QUrl &url, const QSize &size)
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
    const DecodeTarget target{ key.size * dpr, animationLimitMiB * BytesPerMiB };

    // newest first
    // clang-format off
    decodePool.start([this, key, path, target, dpr]() {
        const Decoded decoded = decodeFrames(path, target);
        QMetaObject::invokeMethod(this, [this, key, decoded, dpr]() { publish(key, decoded, dpr); }, Qt::QueuedConnection);
    }, ++newestFirstPriority);
    // clang-format on
}

AnimatedImageCache::Decoded AnimatedImageCache::decodeFrames(const QString &path, const DecodeTarget &target)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = file.readAll();

    if (Lottie::looksLikeLottie(data))
        return decodeLottie(data, target);

    if (const auto apng = Apng::Reader::open(data))
        return decodeApng(*apng, target);

    return decodeWithImageReader(data, target);
}

AnimatedImageCache::Decoded AnimatedImageCache::decodeApng(Apng::Reader &apng, const DecodeTarget &target)
{
    const auto frameSize = frameSizeWithinBudget(apng.canvasSize().scaled(target.physicalSize, Qt::KeepAspectRatio), apng.frameCount(), target.byteBudget);
    if (!frameSize) {
        qCDebug(LogCore) << "APNG exceeds frame budget, rendering static";
        return {};
    }

    Decoded out;
    QImage canvas;
    int delayMs = 0;
    while (apng.next(canvas, delayMs)) {
        out.frames.append(scaledFrame(canvas, *frameSize));
        out.delaysMs.append(normalizedDelay(delayMs));
    }
    return out;
}

AnimatedImageCache::Decoded AnimatedImageCache::decodeLottie(const QByteArray &data, const DecodeTarget &target)
{
    const auto animation = Lottie::Animation::fromJson(data);
    if (!animation)
        return {};

    const auto budgetedSize = frameSizeWithinBudget(target.physicalSize, animation->frameCount(), target.byteBudget);
    if (!budgetedSize)
        qCDebug(LogCore) << "Lottie animation exceeds frame budget, rendering static";
    const int frameCount = budgetedSize ? animation->frameCount() : 1;
    const QSize frameSize = budgetedSize.value_or(target.physicalSize);

    Decoded out;
    for (int frame = 0; frame < frameCount; ++frame) {
        out.frames.append(animation->render(frame, frameSize));
        out.delaysMs.append(animation->frameDelayMs());
    }
    return out;
}

AnimatedImageCache::Decoded AnimatedImageCache::decodeWithImageReader(const QByteArray &data, const DecodeTarget &target)
{
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);

    QImageReader reader(&buffer);
    if (!reader.canRead() || !reader.supportsAnimation())
        return {};

    const int declaredFrames = reader.imageCount();
    const bool frameCountKnown = declaredFrames > 0;

    Decoded out;
    QSize frameSize;
    qsizetype bytes = 0;
    for (;;) {
        const QImage source = reader.read();
        if (source.isNull())
            break;

        const int delay = reader.nextImageDelay();

        if (out.frames.isEmpty()) {
            const QSize wantedSize = source.size().scaled(target.physicalSize, Qt::KeepAspectRatio);
            const auto budgetedSize = frameCountKnown ? frameSizeWithinBudget(wantedSize, declaredFrames, target.byteBudget) : wantedSize;
            if (!budgetedSize) {
                qCDebug(LogCore) << "Animated image exceeds frame budget, rendering static";
                return {};
            }
            frameSize = *budgetedSize;
        }

        const QImage frame = scaledFrame(source, frameSize);

        bytes += frame.sizeInBytes();
        if (bytes > target.byteBudget || out.frames.size() >= MaxFrames) {
            qCDebug(LogCore) << "Animated image exceeds frame budget, rendering static";
            return {};
        }

        out.frames.append(frame);
        out.delaysMs.append(normalizedDelay(delay));
    }
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
