#include "Core/Video/ScreenPipeline.hpp"

#include "Core/Logging.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/EncodeThread.hpp"
#include "Core/Video/ScreenCapture.hpp"

#include <QElapsedTimer>

#include <atomic>
#include <utility>
#include <vector>

namespace Acheron {
namespace Core {
namespace Video {

namespace {

using Discord::Voice::EncodedVideoFrame;
using Discord::Voice::VideoCodec;

constexpr int PREVIEW_INTERVAL_MS = 500;
constexpr QSize PREVIEW_BOUND(512, 288);
constexpr int STATS_INTERVAL_MS = 1000;
// a window being closed is hidden for a moment first; that ends the share rather than pausing it
constexpr int HIDDEN_BEFORE_PAUSED_MS = 200;

QString noEncoderFor(VideoCodec codec)
{
    return ScreenPipeline::tr("No %1 encoder works on this machine.").arg(Discord::Voice::videoCodecName(codec));
}

} // namespace

class ScreenPipeline::Worker
{
public:
    struct Start
    {
        ScreenSettings settings;
        CaptureMethod method = CaptureMethod::Auto;
    };

    explicit Worker(ScreenPipeline *pipeline) : pipeline(pipeline) {}

    std::atomic<bool> keyframeWanted{ false };
    std::atomic<bool> suspended{ false };
    std::atomic<int> bitrateBps{ 0 };

private:
    void capture(const ScreenSettings &settings, CaptureMethod method, quint64 session);

    ScreenPipeline *pipeline;
    RtpClock clock;

public:
    CaptureThread<Start> thread{ [this](const Start &start, quint64 session) { capture(start.settings, start.method, session); } };
};

void ScreenPipeline::Worker::capture(const ScreenSettings &settings, CaptureMethod method, quint64 session)
{
    Media::VideoEncoder::Settings encoding;
    encoding.codec = settings.codec;
    encoding.framerate = std::max(1, settings.framerate);
    encoding.bitrateBps = usableBitrate(bitrateBps.load(), settings.maxBitrateBps);
    encoding.content = Media::VideoEncoder::Content::Screen;
    Media::VideoEncoder encoder(encoding);
    if (!encoder.hasEncoder()) {
        emit pipeline->failed(noEncoderFor(settings.codec));
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
    request.bt601Only = Discord::Voice::carriesOnlyBt601(settings.codec);
    ScreenCapture screen;
    screen.configure(request);
    screen.setStillWanted([this, session] { return thread.isCurrent(session); });

    const Media::FramePointer frame(av_frame_alloc());
    const Media::FramePointer kept(av_frame_alloc());
    PreviewMaker previews(PREVIEW_BOUND);
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
    EncodeCounters counters;
    int capturedLately = 0;

    while (thread.isCurrent(session)) {
        if (!request.keepOnCard && encoder.takesCardFrames()) {
            request.keepOnCard = true;
            screen.configure(request);
        }

        const bool suspendedNow = suspended;
        const bool pictureWanted = !suspendedNow || !announcedSize.isValid() || !sincePreview.isValid() || sincePreview.elapsed() >= PREVIEW_INTERVAL_MS;
        screen.setPicturesWanted(pictureWanted);
        const ScreenCapture::Status status = screen.read(frame.get());
        if (!thread.isCurrent(session))
            return;
        if (status == ScreenCapture::Status::Failed) {
            emit pipeline->failed(screen.failure());
            return;
        }
        if (status == ScreenCapture::Status::Closed) {
            emit pipeline->sourceClosed();
            return;
        }

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

        const int64_t timestamp = clock.timestampOf(std::chrono::steady_clock::now());
        const QSize size(picture->width, picture->height);
        capturedLately++;

        const QSize codedSize = encoder.codedSize(size);
        const bool heldBack = !Media::paddingIsAcceptable(size, codedSize);

        const bool keyframeAsked = keyframeWanted.exchange(false);
        if (!suspendedNow && !heldBack) {
            encoder.setBitrate(usableBitrate(bitrateBps.load(), settings.maxBitrateBps));
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
                emit pipeline->failed(noEncoderFor(settings.codec));
                return;
            }
            if (outcome == Media::VideoEncoder::Outcome::SizeRefused && size != announcedSize)
                qCInfo(LogVideo) << "The encoder will not take" << size << "; nothing is sent until the source changes size again";
        }
        wasSuspended = suspendedNow;

        if (std::exchange(announcedSize, size) != size) {
            if (heldBack)
                qCInfo(LogVideo) << "Not encoding" << Discord::Voice::videoCodecName(settings.codec) << "at" << size << "which its encoder would code as" << codedSize;
            emit pipeline->started(size, encoding.framerate, 0);
        }

        if (!suspendedNow && !heldBack)
            counters.deliver(encoded, pipeline, 0);

        if (!sincePreview.isValid() || sincePreview.elapsed() >= PREVIEW_INTERVAL_MS) {
            sincePreview.start();
            if (!std::exchange(previewIsOfThisPicture, true))
                preview = previews.make(picture);
            if (!preview.isNull())
                emit pipeline->previewReady(preview);
        }

        if (sinceStats.elapsed() >= STATS_INTERVAL_MS) {
            const double seconds = static_cast<double>(sinceStats.restart()) / 1000.0;
            const QString encoderName = heldBack ? QStringLiteral("none while %1 would add black to this size").arg(Discord::Voice::videoCodecName(settings.codec)) : encoderOrNone(encoder.stats());
            emit pipeline->statsUpdated(counters.takeStats(seconds, encoder.stats(), encoderName, screen.captureName(), size, capturedLately), 0);
            capturedLately = 0;
        }

        av_frame_unref(frame.get());
    }
}

ScreenPipeline::ScreenPipeline(QObject *parent) : EncodePipeline(parent), worker(std::make_unique<Worker>(this)) {}

ScreenPipeline::~ScreenPipeline() = default;

void ScreenPipeline::start(const ScreenSettings &settings)
{
    worker->bitrateBps = usableBitrate(settings.bitrateBps, settings.maxBitrateBps);
    worker->thread.start({ settings, captureMethod() });
}

void ScreenPipeline::stop()
{
    worker->thread.stop();
}

void ScreenPipeline::requestKeyframe()
{
    worker->keyframeWanted = true;
}

void ScreenPipeline::setBitrate(int bitrateBps)
{
    worker->bitrateBps = bitrateBps;
}

void ScreenPipeline::setEncodingSuspended(bool suspended)
{
    worker->suspended = suspended;
}

} // namespace Video
} // namespace Core
} // namespace Acheron
