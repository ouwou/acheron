#include "Core/Video/BroadcastPipeline.hpp"

#include "Core/Logging.hpp"
#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Media/RgbScaler.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/ScreenCapture.hpp"

#include <QElapsedTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

namespace Acheron {
namespace Core {
namespace Video {

namespace {

using Discord::Voice::EncodedVideoFrame;
using Discord::Voice::VideoCodec;

constexpr int PREVIEW_INTERVAL_MS = 500;
constexpr QSize PREVIEW_BOUND(512, 288);
constexpr int STATS_INTERVAL_MS = 1000;
// How long a window has to have been hidden before its share is paused. One that is being
// closed is hidden for a moment first, and that is the end of the share and not a pause.
constexpr int HIDDEN_BEFORE_PAUSED_MS = 200;
constexpr int MIN_BITRATE = 50'000;
constexpr int BITRATE_WHEN_NONE_GIVEN = 2'500'000;

using Media::FramePointer;

int usableBitrate(int wanted, int most)
{
    if (wanted <= 0)
        wanted = BITRATE_WHEN_NONE_GIVEN;
    if (most > 0)
        wanted = std::min(wanted, most);
    return std::max(wanted, MIN_BITRATE);
}

// Small stills of what is being captured, each in an image of its own.
class PreviewMaker
{
public:
    QImage make(const AVFrame *frame)
    {
        const AVFrame *picture = frame;
        if (frame->hw_frames_ctx) {
            av_frame_unref(downloaded.get());
            if (av_hwframe_transfer_data(downloaded.get(), frame, 0) < 0)
                return {};
            av_frame_copy_props(downloaded.get(), frame);
            picture = downloaded.get();
        }

        const QSize full(picture->width, picture->height);
        const bool fits = full.width() <= PREVIEW_BOUND.width() && full.height() <= PREVIEW_BOUND.height();
        const QSize size = fits ? full : full.scaled(PREVIEW_BOUND, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        if (image.size() != size)
            image = Media::RgbScaler::allocate(size, QImage::Format_RGB32);
        const bool scaled = scaler.scale(picture, image);
        av_frame_unref(downloaded.get());
        return scaled ? image : QImage();
    }

private:
    Media::RgbScaler scaler;
    QImage image;
    FramePointer downloaded{ av_frame_alloc() };
};

} // namespace

class BroadcastPipeline::Worker
{
public:
    explicit Worker(BroadcastPipeline *pipeline) : pipeline(pipeline), thread([this] { run(); }) {}

    ~Worker()
    {
        {
            std::lock_guard lock(mutex);
            quitting = true;
            generation++;
        }
        wake.notify_all();
        thread.join();
    }

    void start(const BroadcastSettings &settings)
    {
        const CaptureMethod method = captureMethod();
        {
            std::lock_guard lock(mutex);
            wanted = settings;
            wantedMethod = method;
            bitrate = settings.bitrate;
            generation++;
        }
        wake.notify_all();
    }

    void stop()
    {
        std::lock_guard lock(mutex);
        wanted.reset();
        generation++;
    }

    std::atomic<bool> keyframeWanted{ false };
    std::atomic<bool> suspended{ false };
    std::atomic<int> bitrate{ 0 };

private:
    void run();
    void capture(const BroadcastSettings &settings, CaptureMethod method, quint64 session);
    [[nodiscard]] int64_t nextTimestamp();

    BroadcastPipeline *pipeline;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<BroadcastSettings> wanted;
    CaptureMethod wantedMethod = CaptureMethod::Auto;
    bool quitting = false;
    // Moves on with every start and stop; a capture ends when it is no longer its own.
    std::atomic<quint64> generation{ 0 };

    // Timestamps run on from one capture to the next, so that a restarted one never goes back.
    const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    int64_t lastTimestamp = -1;

    std::thread thread;
};

void BroadcastPipeline::Worker::run()
{
    for (;;) {
        BroadcastSettings settings;
        CaptureMethod method = CaptureMethod::Auto;
        quint64 session = 0;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [this] { return quitting || wanted; });
            if (quitting)
                return;
            settings = *wanted;
            method = wantedMethod;
            wanted.reset();
            session = generation;
        }
        capture(settings, method, session);
    }
}

int64_t BroadcastPipeline::Worker::nextTimestamp()
{
    const int64_t microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - origin).count();
    lastTimestamp = std::max(lastTimestamp + 1, microseconds * 9 / 100);
    return lastTimestamp;
}

void BroadcastPipeline::Worker::capture(const BroadcastSettings &settings, CaptureMethod method, quint64 session)
{
    Media::VideoEncoder::Settings encoding;
    encoding.codec = settings.codec;
    encoding.framerate = std::max(1, settings.framerate);
    encoding.bitrate = usableBitrate(bitrate, settings.maxBitrate);
    encoding.keyframeIntervalMs = settings.keyframeIntervalMs;
    encoding.content = Media::VideoEncoder::Content::Screen;
    Media::VideoEncoder encoder(encoding);
    if (!encoder.hasEncoder()) {
        emit pipeline->failed(BroadcastPipeline::tr("No %1 encoder works on this machine.").arg(Discord::Voice::videoCodecName(settings.codec)));
        return;
    }

    ScreenCapture::Request request;
    request.sourceId = settings.sourceId;
    request.method = method;
    request.maxHeight = settings.maxHeight;
    request.framerate = encoding.framerate;
    request.softwareFormat = encoder.softwareFormat();
    request.keepOnCard = encoder.takesCardFrames();
    request.rgbOnCard = encoder.takesRgbCardFrames();
    request.bt601Only = settings.codec == VideoCodec::VP8;
    ScreenCapture screen;
    screen.configure(request);

    const FramePointer frame(av_frame_alloc());
    // The last picture of a source that has one only when it shows something new.
    const FramePointer kept(av_frame_alloc());
    PreviewMaker previews;
    QImage preview;
    bool previewIsOfThisPicture = false;
    std::vector<EncodedVideoFrame> encoded;
    QElapsedTimer sincePicture;
    QElapsedTimer sincePreview;
    QElapsedTimer sinceStats;
    sinceStats.start();

    QSize announcedSize;
    bool stalled = false;
    QElapsedTimer hiddenFor;
    bool wasSuspended = false;
    quint64 framesEncoded = 0;
    quint64 framesDropped = 0;
    int capturedLately = 0;
    int encodedLately = 0;
    quint64 bytesLately = 0;

    while (generation == session) {
        // With nobody watching, a picture is only wanted for a preview.
        const bool suspendedNow = suspended;
        const bool pictureWanted = !suspendedNow || !announcedSize.isValid() || !sincePreview.isValid() || sincePreview.elapsed() >= PREVIEW_INTERVAL_MS;
        screen.setPicturesWanted(pictureWanted);
        const ScreenCapture::Status status = screen.read(frame.get());
        if (status == ScreenCapture::Status::Failed) {
            emit pipeline->failed(screen.failure());
            return;
        }
        if (status == ScreenCapture::Status::Closed) {
            emit pipeline->sourceClosed();
            return;
        }

        // A hidden window is known to show nothing, also before its first picture. A source
        // that cannot be read has until its first picture to settle before that is a stall.
        const bool hidden = status == ScreenCapture::Status::Hidden;
        if (hidden && !hiddenFor.isValid())
            hiddenFor.start();
        bool stalledNow = stalled;
        if (hidden)
            stalledNow = stalled || hiddenFor.elapsed() >= HIDDEN_BEFORE_PAUSED_MS;
        else if (status == ScreenCapture::Status::Stalled)
            stalledNow = stalled || announcedSize.isValid();
        else if (status == ScreenCapture::Status::Frame || hiddenFor.isValid())
            stalledNow = false;
        if (!hidden)
            hiddenFor.invalidate();
        if (std::exchange(stalled, stalledNow) != stalledNow)
            emit pipeline->captureStalled(stalledNow);

        if (!pictureWanted)
            continue;

        AVFrame *picture = nullptr;
        if (status == ScreenCapture::Status::Frame)
            previewIsOfThisPicture = false;
        if (status == ScreenCapture::Status::Frame && screen.deliversOnlyChanges()) {
            av_frame_unref(kept.get());
            av_frame_move_ref(kept.get(), frame.get());
            picture = kept.get();
        } else if (status == ScreenCapture::Status::Frame) {
            picture = frame.get();
        } else if (status == ScreenCapture::Status::NoFrame && kept->buf[0] && screen.deliversOnlyChanges()) {
            const bool keyFrameWaitedFor = keyframeWanted || (wasSuspended && !suspendedNow);
            if (stillPictureIsDue(sincePicture.elapsed(), keyFrameWaitedFor, encoding.framerate))
                picture = kept.get();
        }
        if (!picture)
            continue;
        sincePicture.start();

        const int64_t timestamp = nextTimestamp();
        const QSize size(picture->width, picture->height);
        capturedLately++;

        // Nothing is encoded of a size the encoder would add too much black to. The size is
        // still reported, and it is for whoever started this to have another codec chosen.
        const QSize codedSize = encoder.codedSize(size);
        const bool heldBack = !Media::paddingIsAcceptable(size, codedSize);

        const bool keyframeAsked = keyframeWanted.exchange(false);
        if (!suspendedNow && !heldBack) {
            encoder.setBitrate(usableBitrate(bitrate, settings.maxBitrate));
            if (keyframeAsked || wasSuspended)
                encoder.requestKeyframe();

            encoded.clear();
            const Media::VideoEncoder::Outcome outcome = encoder.encode(picture, timestamp, encoded);
            if (outcome == Media::VideoEncoder::Outcome::CardFramesRefused) {
                qCInfo(LogVideo) << "The encoder does not take pictures that are on the graphics card; reading them back into memory";
                request.keepOnCard = false;
                screen.configure(request);
                av_frame_unref(frame.get());
                av_frame_unref(kept.get());
                continue;
            }
            if (outcome == Media::VideoEncoder::Outcome::Failed) {
                emit pipeline->failed(BroadcastPipeline::tr("No %1 encoder works on this machine.").arg(Discord::Voice::videoCodecName(settings.codec)));
                return;
            }
        }
        wasSuspended = suspendedNow;

        // Again whenever a window has changed its size, which the picture follows.
        if (std::exchange(announcedSize, size) != size) {
            if (heldBack)
                qCInfo(LogVideo) << "Not encoding" << Discord::Voice::videoCodecName(settings.codec) << "at" << size << "which its encoder would code as" << codedSize;
            emit pipeline->started(size, encoding.framerate);
        }

        if (!suspendedNow && !heldBack) {
            framesDropped += encoded.empty() ? 1 : 0;
            for (EncodedVideoFrame &each : encoded) {
                framesEncoded++;
                encodedLately++;
                bytesLately += each.data.size();
                emit pipeline->frameEncoded(std::make_shared<const EncodedVideoFrame>(std::move(each)));
            }
        }

        if (!sincePreview.isValid() || sincePreview.elapsed() >= PREVIEW_INTERVAL_MS) {
            sincePreview.start();
            // A picture sent again because nothing new has come is the one the last preview is of.
            if (!std::exchange(previewIsOfThisPicture, true))
                preview = previews.make(picture);
            if (!preview.isNull())
                emit pipeline->previewReady(preview);
        }

        if (sinceStats.elapsed() >= STATS_INTERVAL_MS) {
            const double seconds = static_cast<double>(sinceStats.restart()) / 1000.0;
            const Media::VideoEncoder::Stats &encoderStats = encoder.stats();
            BroadcastStats stats;
            // The capture goes into the name for want of a place of its own in the statistics.
            QString encoderName = encoderStats.encoderName.isEmpty() ? QStringLiteral("no encoder yet") : encoderStats.encoderName;
            if (heldBack)
                encoderName = QStringLiteral("none while %1 would add black to this size").arg(Discord::Voice::videoCodecName(settings.codec));
            stats.encoderName = QStringLiteral("%1, from %2").arg(encoderName, screen.captureName());
            stats.hardware = encoderStats.hardware;
            stats.encodedSize = size;
            stats.capturedFps = capturedLately / seconds;
            stats.encodedFps = encodedLately / seconds;
            stats.averageEncodeMs = encoderStats.averageEncodeMs;
            stats.bitrateBps = static_cast<quint64>(static_cast<double>(bytesLately) * 8.0 / seconds);
            stats.framesEncoded = framesEncoded;
            stats.framesDropped = framesDropped;
            emit pipeline->statsUpdated(stats);
            capturedLately = 0;
            encodedLately = 0;
            bytesLately = 0;
        }

        av_frame_unref(frame.get());
    }
}

BroadcastPipeline::BroadcastPipeline(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<EncodedFramePointer>("Acheron::Core::Video::EncodedFramePointer");
    qRegisterMetaType<BroadcastStats>("Acheron::Core::Video::BroadcastStats");
    worker = std::make_unique<Worker>(this);
}

BroadcastPipeline::~BroadcastPipeline() = default;

void BroadcastPipeline::start(const BroadcastSettings &settings)
{
    worker->start(settings);
}

void BroadcastPipeline::stop()
{
    worker->stop();
}

void BroadcastPipeline::requestKeyframe()
{
    worker->keyframeWanted = true;
}

void BroadcastPipeline::setBitrate(int bitrate)
{
    worker->bitrate = bitrate;
}

void BroadcastPipeline::setEncodingSuspended(bool suspended)
{
    worker->suspended = suspended;
}

} // namespace Video
} // namespace Core
} // namespace Acheron
