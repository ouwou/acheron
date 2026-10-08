#include "Core/Media/ClipPlayer.hpp"

#include "Core/Media/ClipDecoder.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace Acheron {
namespace Core {
namespace Media {

namespace {

constexpr qint64 MinFrameGapMs = 10;
constexpr qint64 MaxFrameGapMs = 60 * 1000;
constexpr int FramesInAnAnimation = 2;
constexpr int BytesPerPixel = 4;
constexpr int FrameBuffersPerClip = 2;
constexpr int NativeSizedCopiesInsideADecoder = 2;

qint64 pixelCount(const QSize &size)
{
    return qint64(size.width()) * size.height();
}

QSize frameSizeWithoutUpscaling(const QSize &native, const QSize &boundPixels, Qt::AspectRatioMode fit)
{
    const QSize scaled = native.scaled(boundPixels, fit);
    if (scaled.isEmpty() || scaled.width() >= native.width() || scaled.height() >= native.height())
        return native;
    return scaled;
}

} // namespace

struct ClipPlayer::Clip
{
    QString path;
    QSize boundPixels;
    Qt::AspectRatioMode fit = Qt::KeepAspectRatio;
    qint64 memoryLimitBytes = 0;

    std::unique_ptr<ClipDecoder> decoder;
    QSize frameSize;
    qint64 previousStartMs = 0;
    int framesThisLoop = 0;

    bool playing = false;
    bool failed = false;
    bool hasReady = false;
    QImage ready;
    qint64 readyGapMs = 0;
    QImage spare;

    QImage shown;
    std::optional<qint64> shownAtMs;
    bool failureReported = false;
};

ClipPlayer::ClipPlayer(QObject *parent) : QObject(parent)
{
    clock.start();
    presentTimer.setSingleShot(true);
    connect(&presentTimer, &QTimer::timeout, this, &ClipPlayer::presentDueFrames);
}

ClipPlayer::~ClipPlayer()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    frameWanted.notify_all();
    if (decodeThread.joinable())
        decodeThread.join();
}

ClipPlayer::ClipId ClipPlayer::open(const QString &path, const QSize &boundPixels, Qt::AspectRatioMode fit)
{
    auto clip = std::make_shared<Clip>();
    clip->path = path;
    clip->boundPixels = boundPixels;
    clip->fit = fit;
    clip->memoryLimitBytes = newClipMemoryLimitBytes;

    const ClipId id = ++lastId;
    clips.insert(id, clip);
    {
        std::lock_guard<std::mutex> lock(mutex);
        decodeOrder.push_back(clip);
    }

    if (!decodeThread.joinable())
        decodeThread = std::thread(&ClipPlayer::decodeLoop, this);
    return id;
}

qint64 ClipPlayer::memoryToPlay(const QSize &nativeSize, const QSize &frameSize)
{
    return (pixelCount(nativeSize) * NativeSizedCopiesInsideADecoder + pixelCount(frameSize) * FrameBuffersPerClip) * BytesPerPixel;
}

void ClipPlayer::setMemoryLimitForNewClips(qint64 bytes)
{
    newClipMemoryLimitBytes = bytes;
}

void ClipPlayer::close(ClipId id)
{
    const ClipPtr clip = clips.take(id);
    if (!clip)
        return;

    std::lock_guard<std::mutex> lock(mutex);
    decodeOrder.erase(std::remove(decodeOrder.begin(), decodeOrder.end(), clip), decodeOrder.end());
}

void ClipPlayer::closeAll()
{
    clips.clear();
    presentTimer.stop();

    std::lock_guard<std::mutex> lock(mutex);
    decodeOrder.clear();
}

void ClipPlayer::setPlaying(ClipId id, bool playing)
{
    const auto it = clips.constFind(id);
    if (it == clips.constEnd())
        return;

    Clip &clip = **it;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (clip.playing == playing)
            return;
        clip.playing = playing;
    }

    if (!playing)
        return;

    if (clip.shownAtMs)
        clip.shownAtMs = clock.elapsed();
    frameWanted.notify_one();
    presentDueFrames();
}

QImage ClipPlayer::frame(ClipId id) const
{
    const auto it = clips.constFind(id);
    return it != clips.constEnd() ? (*it)->shown : QImage();
}

ClipPlayer::ClipPtr ClipPlayer::nextClipAwaitingFrame()
{
    for (size_t offset = 0; offset < decodeOrder.size(); ++offset) {
        const size_t slot = (nextToServe + offset) % decodeOrder.size();
        const ClipPtr &clip = decodeOrder[slot];
        if (clip->playing && !clip->failed && !clip->hasReady) {
            nextToServe = slot + 1;
            return clip;
        }
    }
    return nullptr;
}

std::optional<qint64> ClipPlayer::decodeNextFrame(Clip &clip, QImage &buffer)
{
    if (!clip.decoder) {
        clip.decoder = ClipDecoder::open(clip.path);
        if (!clip.decoder)
            return std::nullopt;
        const QSize native = clip.decoder->nativeSize();
        clip.frameSize = frameSizeWithoutUpscaling(native, clip.boundPixels, clip.fit);
        if (clip.frameSize.isEmpty() || memoryToPlay(native, clip.frameSize) > clip.memoryLimitBytes) {
            clip.decoder.reset();
            return std::nullopt;
        }
    }

    qint64 startMs = 0;
    qint64 gapMs = 0;
    if (clip.decoder->nextFrameOfLoop(buffer, startMs, clip.frameSize)) {
        const bool firstFrameEver = clip.framesThisLoop == 0;
        gapMs = firstFrameEver ? 0 : startMs - clip.previousStartMs;
    } else {
        if (clip.framesThisLoop < FramesInAnAnimation)
            return std::nullopt;

        const qint64 lastFrameShownForMs = std::max<qint64>(0, clip.decoder->finishedLoopMs() - clip.previousStartMs);
        clip.framesThisLoop = 0;
        if (!clip.decoder->rewind() || !clip.decoder->nextFrameOfLoop(buffer, startMs, clip.frameSize))
            return std::nullopt;
        gapMs = lastFrameShownForMs + startMs;
    }

    clip.previousStartMs = startMs;
    ++clip.framesThisLoop;
    return std::clamp(gapMs, MinFrameGapMs, MaxFrameGapMs);
}

void ClipPlayer::decodeLoop()
{
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
        ClipPtr clip;
        frameWanted.wait(lock, [this, &clip] {
            if (!stopping)
                clip = nextClipAwaitingFrame();
            return stopping || clip;
        });
        if (stopping)
            return;

        QImage buffer = std::exchange(clip->spare, QImage());
        lock.unlock();

        const std::optional<qint64> gapMs = decodeNextFrame(*clip, buffer);

        lock.lock();
        if (gapMs) {
            clip->ready = std::move(buffer);
            clip->readyGapMs = *gapMs;
            clip->hasReady = true;
        } else {
            clip->failed = true;
        }
        lock.unlock();

        clip.reset();
        requestPresent();

        lock.lock();
    }
}

void ClipPlayer::requestPresent()
{
    if (presentRequested.exchange(true))
        return;

    // clang-format off
    QMetaObject::invokeMethod(this, [this]() {
        presentRequested.store(false);
        presentDueFrames();
    }, Qt::QueuedConnection);
    // clang-format on
}

void ClipPlayer::presentDueFrames()
{
    const qint64 now = clock.elapsed();
    qint64 nextDueMs = std::numeric_limits<qint64>::max();
    QList<ClipId> changed;
    QList<ClipId> failed;

    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = clips.constBegin(); it != clips.constEnd(); ++it) {
            Clip &clip = **it;
            if (clip.failed) {
                if (!std::exchange(clip.failureReported, true))
                    failed.append(it.key());
                continue;
            }
            if (!clip.playing || !clip.hasReady)
                continue;

            const qint64 dueMs = clip.shownAtMs ? *clip.shownAtMs + clip.readyGapMs : now;
            if (dueMs > now) {
                nextDueMs = std::min(nextDueMs, dueMs);
                continue;
            }

            clip.spare = std::exchange(clip.shown, std::exchange(clip.ready, QImage()));
            clip.hasReady = false;

            const bool fellBehind = now - dueMs > clip.readyGapMs;
            clip.shownAtMs = fellBehind ? now : dueMs;
            changed.append(it.key());
        }
    }

    if (!changed.isEmpty())
        frameWanted.notify_one();

    if (nextDueMs != std::numeric_limits<qint64>::max())
        presentTimer.start(int(nextDueMs - now));

    for (const ClipId id : std::as_const(failed))
        emit clipFailed(id);
    for (const ClipId id : std::as_const(changed))
        emit frameChanged(id);
}

} // namespace Media
} // namespace Core
} // namespace Acheron
