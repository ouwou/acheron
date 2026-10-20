#include "Core/Video/CameraPipeline.hpp"

#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Media/RgbScaler.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/CameraCapture.hpp"
#include "Core/Video/EncodeThread.hpp"

#include <QElapsedTimer>
#include <QSettings>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace Acheron {
namespace Core {
namespace Video {

namespace {

using Discord::Voice::EncodedVideoFrame;
using Discord::Voice::VideoCodec;
using Clock = std::chrono::steady_clock;
using Media::FramePointer;

constexpr int PREVIEWS_PER_SECOND = 15;
constexpr QSize PREVIEW_BOUND(640, 360);
constexpr int STATS_INTERVAL_MS = 1000;

struct Encoding
{
    QSize size;
    int framerate = 30;
};

struct Asked
{
    int bitrateBps = 0;
    // Set when a size and frame rate were asked for, also the ones there already are.
    std::optional<Encoding> encoding;
};

// The camera's picture inside the size to encode: never stretched and never enlarged, so a
// camera of another shape is sent smaller than asked, and both sides even.
QSize fittedInside(const QSize &camera, const QSize &bound)
{
    QSize size = camera;
    if (!bound.isEmpty() && (camera.width() > bound.width() || camera.height() > bound.height()))
        size = camera.scaled(bound, Qt::KeepAspectRatio);
    return QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
}

// Lets through as many pictures a second as are wanted, of a camera that takes more.
class FrameThinner
{
public:
    explicit FrameThinner(int framerate) : interval(std::chrono::microseconds(1'000'000 / std::max(1, framerate))) {}

    bool takes(Clock::time_point capturedAt)
    {
        // A camera's pictures come a little early as often as a little late.
        if (due && capturedAt < *due - interval / 4)
            return false;
        due = std::max(due ? *due + interval : capturedAt + interval, capturedAt);
        return true;
    }

private:
    Clock::duration interval;
    std::optional<Clock::time_point> due;
};

// Makes of the camera's picture the one that is encoded: at the size to send, in the format
// the encoder takes, and in the colours a viewer takes a stream to be in when it does not say:
// video range, where a camera that sends MJPEG sends full range, and BT.709 from 720 lines up
// and BT.601 below. The picture says so as well, but not every encoder passes that on (AMF
// leaves BT.601 unsaid), and VP8 has no way to: it is BT.601 by definition.
class EncodePictureMaker
{
public:
    EncodePictureMaker() = default;
    ~EncodePictureMaker() { sws_freeContext(context); }

    EncodePictureMaker(const EncodePictureMaker &) = delete;
    EncodePictureMaker &operator=(const EncodePictureMaker &) = delete;

    // Null when the picture could not be converted. `camera` itself when it is already as wanted.
    AVFrame *make(AVFrame *camera, const QSize &size, int format, bool bt601Only)
    {
        const Conversion wanted{ camera->width, camera->height, camera->format, Media::usesBt709(camera), Media::isFullRange(camera), size, format, !bt601Only && Media::usesBt709AtHeight(size.height()) };
        if (wanted.changesNothing())
            return camera;
        if (wanted != configuredFor && !configure(wanted))
            return nullptr;

        if (made->width != size.width() || made->height != size.height() || made->format != format) {
            av_frame_unref(made.get());
            made->width = size.width();
            made->height = size.height();
            made->format = format;
            if (av_frame_get_buffer(made.get(), 0) < 0)
                return nullptr;
        }
        // An encoder may still hold the last picture.
        if (av_frame_make_writable(made.get()) < 0)
            return nullptr;
        if (sws_scale(context, camera->data, camera->linesize, 0, camera->height, made->data, made->linesize) <= 0)
            return nullptr;

        made->colorspace = wanted.toBt709 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        made->color_range = AVCOL_RANGE_MPEG;
        made->color_primaries = camera->color_primaries;
        made->color_trc = camera->color_trc;
        return made.get();
    }

private:
    struct Conversion
    {
        int width = 0;
        int height = 0;
        int format = -1;
        bool fromBt709 = false;
        bool fromFullRange = false;
        QSize size;
        int toFormat = -1;
        bool toBt709 = false;

        bool operator==(const Conversion &) const = default;
        [[nodiscard]] bool changesNothing() const { return size == QSize(width, height) && toFormat == format && toBt709 == fromBt709 && !fromFullRange; }
    };

    bool configure(const Conversion &wanted)
    {
        sws_freeContext(context);
        context = sws_alloc_context();
        if (!context)
            return false;
        configuredFor = {};

        // The ranges are set before the context is made ready: swscale picks a plain copy for
        // two formats that differ only in their layout, and that does not convert a range it
        // is told of afterwards.
        av_opt_set_int(context, "srcw", wanted.width, 0);
        av_opt_set_int(context, "srch", wanted.height, 0);
        av_opt_set_int(context, "src_format", wanted.format, 0);
        av_opt_set_int(context, "src_range", wanted.fromFullRange ? 1 : 0, 0);
        av_opt_set_int(context, "dstw", wanted.size.width(), 0);
        av_opt_set_int(context, "dsth", wanted.size.height(), 0);
        av_opt_set_int(context, "dst_format", wanted.toFormat, 0);
        av_opt_set_int(context, "dst_range", 0, 0);
        av_opt_set_int(context, "sws_flags", SWS_BILINEAR, 0);
        if (sws_init_context(context, nullptr, nullptr) < 0)
            return false;

        sws_setColorspaceDetails(context, sws_getCoefficients(wanted.fromBt709 ? SWS_CS_ITU709 : SWS_CS_ITU601), wanted.fromFullRange ? 1 : 0, sws_getCoefficients(wanted.toBt709 ? SWS_CS_ITU709 : SWS_CS_ITU601), 0, 0, 1 << 16, 1 << 16);
        configuredFor = wanted;
        return true;
    }

    SwsContext *context = nullptr;
    Conversion configuredFor;
    FramePointer made{ av_frame_alloc() };
};

Media::VideoEncoder::Settings encoderSettings(const CameraSettings &settings, int framerate, int bitrateBps)
{
    Media::VideoEncoder::Settings encoding;
    encoding.codec = settings.codec;
    encoding.framerate = framerate;
    encoding.bitrate = bitrateBps;
    encoding.keyframeIntervalMs = settings.keyframeIntervalMs;
    encoding.content = Media::VideoEncoder::Content::Camera;
    return encoding;
}

} // namespace

QString preferredCamera(const QList<CameraDevice> &attached)
{
    const QString chosen = QSettings().value(QStringLiteral("video/camera_device")).toString();
    const bool stillAttached = std::any_of(attached.cbegin(), attached.cend(), [&chosen](const CameraDevice &device) { return device.id == chosen; });
    return stillAttached ? chosen : QString();
}

void setPreferredCamera(const QString &deviceId)
{
    QSettings().setValue(QStringLiteral("video/camera_device"), deviceId);
}

class CameraPipeline::Worker
{
public:
    explicit Worker(CameraPipeline *pipeline) : pipeline(pipeline)
    {
        // Every layer but the first waits to be asked for.
        for (int layer = 1; layer < LAYERS; layer++)
            suspended[static_cast<size_t>(layer)] = true;
    }

    void start(const CameraSettings &settings)
    {
        thread.change([this](std::optional<CameraSettings> &) { requests[0].encodingWanted.reset(); });
        thread.start(settings);
    }

    // A camera that has not been opened yet opens with what is asked for here.
    void setBitrate(int bitrateBps, int layer)
    {
        thread.change([&](std::optional<CameraSettings> &pending) {
            Requests &asked = requests[static_cast<size_t>(layer)];
            if (pending && layer == 0) {
                pending->bitrateBps = bitrateBps;
            } else if (asked.bitrateBps != bitrateBps) {
                asked.bitrateBps = bitrateBps;
                asked.changes++;
            }
        });
    }

    void setEncoding(const QSize &size, int framerate, int layer)
    {
        thread.change([&](std::optional<CameraSettings> &pending) {
            Requests &asked = requests[static_cast<size_t>(layer)];
            if (pending && layer == 0) {
                pending->encodeSize = size;
                pending->framerate = framerate;
            } else {
                asked.encodingWanted = Encoding{ size, std::max(1, framerate) };
                asked.changes++;
            }
        });
    }

    std::array<std::atomic<bool>, LAYERS> keyframeWanted{};
    std::array<std::atomic<bool>, LAYERS> suspended{};

private:
    // What has been asked of one layer since its last picture, guarded by the thread's lock.
    struct Requests
    {
        int bitrateBps = 0;
        std::optional<Encoding> encodingWanted;
        quint64 changes = 0;
        quint64 changesAtLastPicture = 0;
    };

    // One encoding of the camera's pictures, as it stands in a capture.
    struct Layer
    {
        explicit Layer(const Encoding &encoding) : encoding(encoding), thinner(encoding.framerate) {}

        Encoding encoding;
        int sendingBitrateBps = 0;
        // Made when the layer is first asked for.
        std::optional<Media::VideoEncoder> encoder;
        EncodePictureMaker pictures;
        FrameThinner thinner;
        std::optional<Encoding> announced;
        bool wasSuspended = false;
        quint64 framesEncoded = 0;
        quint64 framesDropped = 0;
        int encodedLately = 0;
        quint64 bytesLately = 0;
    };

    void capture(const CameraSettings &settings, quint64 session);
    [[nodiscard]] std::optional<Asked> takeAsked(int layer);

    CameraPipeline *pipeline;
    std::array<Requests, LAYERS> requests;
    RtpClock clock;

public:
    CaptureThread<CameraSettings> thread{ [this](const CameraSettings &settings, quint64 session) { capture(settings, session); },
                                          [this](const CameraSettings &settings) { requests[0].bitrateBps = settings.bitrateBps; } };
};

// What is asked of a layer now, once a picture has come with nothing new asked for since the
// picture before it. A new bit rate and a new size asked for one after the other are each a
// new start for the encoder, and a key frame: taken together like this they are one.
std::optional<Asked> CameraPipeline::Worker::takeAsked(int layer)
{
    std::optional<Asked> taken;
    thread.change([&](std::optional<CameraSettings> &) {
        Requests &asked = requests[static_cast<size_t>(layer)];
        if (asked.changes == std::exchange(asked.changesAtLastPicture, asked.changes))
            taken = Asked{ asked.bitrateBps, std::exchange(asked.encodingWanted, std::nullopt) };
    });
    return taken;
}

void CameraPipeline::Worker::capture(const CameraSettings &settings, quint64 session)
{
    const QString noEncoder = CameraPipeline::tr("No %1 encoder works on this machine.").arg(Discord::Voice::videoCodecName(settings.codec));
    // A layer that has been told nothing of its own yet starts as the first does.
    std::deque<Layer> layers;
    for (int index = 0; index < LAYERS; index++)
        layers.emplace_back(Encoding{ settings.encodeSize, std::max(1, settings.framerate) });
    for (Layer &layer : layers)
        layer.sendingBitrateBps = usableBitrate(settings.bitrateBps, settings.maxBitrateBps);
    layers[0].encoder.emplace(encoderSettings(settings, layers[0].encoding.framerate, layers[0].sendingBitrateBps));
    if (!layers[0].encoder->hasEncoder()) {
        emit pipeline->failed(noEncoder);
        return;
    }

    const std::unique_ptr<CameraFrameSource> camera = makeCameraFrameSource();
    if (!camera || !camera->open(settings.deviceId)) {
        emit pipeline->failed(camera ? camera->failure() : CameraPipeline::tr("This build cannot open cameras."));
        return;
    }

    const FramePointer frame(av_frame_alloc());
    PreviewMaker previews(PREVIEW_BOUND);
    FrameThinner previewThinner(PREVIEWS_PER_SECOND);
    std::vector<EncodedVideoFrame> encoded;
    QElapsedTimer sinceStats;
    sinceStats.start();
    int capturedLately = 0;

    // False when the camera cannot go on, which has then been said.
    const auto encodeLayer = [&](int index, Clock::time_point capturedAt, int64_t timestamp) {
        Layer &layer = layers[static_cast<size_t>(index)];
        const std::optional<Asked> asked = takeAsked(index);
        if (asked && asked->bitrateBps > 0)
            layer.sendingBitrateBps = usableBitrate(asked->bitrateBps, settings.maxBitrateBps);
        // VideoEncoder has no way to be given a new frame rate, so a new one is made, which
        // starts with a key frame as a new size would anyway.
        if (asked && asked->encoding) {
            layer.encoding = *asked->encoding;
            if (layer.encoder)
                layer.encoder.emplace(encoderSettings(settings, layer.encoding.framerate, layer.sendingBitrateBps));
            layer.thinner = FrameThinner(layer.encoding.framerate);
        }

        const bool suspendedNow = suspended[static_cast<size_t>(index)];
        if (!layer.encoder) {
            if (suspendedNow)
                return true;
            layer.encoder.emplace(encoderSettings(settings, layer.encoding.framerate, layer.sendingBitrateBps));
            if (!layer.encoder->hasEncoder()) {
                emit pipeline->failed(noEncoder);
                return false;
            }
        }
        // An encoder that has yet to open waits for what is still being asked for, and opens once.
        if (!asked && layer.encoder->stats().opened == 0)
            return true;
        const Encoding sending{ fittedInside(QSize(frame->width, frame->height), layer.encoding.size), layer.encoding.framerate };
        const auto announce = [this, &layer, &sending, index] {
            if (!layer.announced || layer.announced->size != sending.size || layer.announced->framerate != sending.framerate)
                emit pipeline->started(sending.size, sending.framerate, index);
            layer.announced = sending;
        };

        if (suspendedNow) {
            layer.wasSuspended = true;
            keyframeWanted[static_cast<size_t>(index)] = false;
            announce();
            return true;
        }
        if (!layer.thinner.takes(capturedAt))
            return true;

        AVFrame *picture = layer.pictures.make(frame.get(), sending.size, layer.encoder->softwareFormat(), settings.codec == VideoCodec::VP8);
        if (!picture) {
            emit pipeline->failed(CameraPipeline::tr("The camera's picture could not be converted."));
            return false;
        }

        layer.encoder->setBitrate(layer.sendingBitrateBps);
        if (keyframeWanted[static_cast<size_t>(index)].exchange(false) || std::exchange(layer.wasSuspended, false))
            layer.encoder->requestKeyframe();
        encoded.clear();
        const Media::VideoEncoder::Outcome outcome = layer.encoder->encode(picture, timestamp, encoded);
        if (outcome == Media::VideoEncoder::Outcome::SizeRefused)
            return true;
        if (outcome != Media::VideoEncoder::Outcome::Encoded) {
            emit pipeline->failed(noEncoder);
            return false;
        }
        announce();

        layer.framesDropped += encoded.empty() ? 1 : 0;
        for (EncodedVideoFrame &each : encoded) {
            layer.framesEncoded++;
            layer.encodedLately++;
            layer.bytesLately += each.data.size();
            emit pipeline->frameEncoded(std::make_shared<const EncodedVideoFrame>(std::move(each)), index);
        }
        return true;
    };

    while (thread.isCurrent(session)) {
        if (sinceStats.elapsed() >= STATS_INTERVAL_MS) {
            const double seconds = static_cast<double>(sinceStats.restart()) / 1000.0;
            for (int index = 0; index < LAYERS; index++) {
                Layer &layer = layers[static_cast<size_t>(index)];
                if (!layer.encoder)
                    continue;
                const Media::VideoEncoder::Stats &encoderStats = layer.encoder->stats();
                PipelineStats stats;
                // The camera's mode goes into the name for want of a place of its own in the statistics.
                stats.encoderName = QStringLiteral("%1, from %2").arg(encoderStats.encoderName.isEmpty() ? QStringLiteral("no encoder yet") : encoderStats.encoderName, camera->description());
                stats.hardware = encoderStats.hardware;
                stats.encodedSize = layer.announced ? layer.announced->size : QSize();
                stats.capturedFps = capturedLately / seconds;
                stats.encodedFps = layer.encodedLately / seconds;
                stats.averageEncodeMs = encoderStats.averageEncodeMs;
                stats.bitrateBps = static_cast<quint64>(static_cast<double>(layer.bytesLately) * 8.0 / seconds);
                stats.framesEncoded = layer.framesEncoded;
                stats.framesDropped = layer.framesDropped;
                emit pipeline->statsUpdated(stats, index);
                layer.encodedLately = 0;
                layer.bytesLately = 0;
            }
            capturedLately = 0;
        }

        av_frame_unref(frame.get());
        Clock::time_point capturedAt;
        const CameraFrameSource::Status status = camera->read(frame.get(), capturedAt);
        if (!thread.isCurrent(session))
            return;
        if (status == CameraFrameSource::Status::Ended) {
            emit pipeline->failed(camera->failure());
            return;
        }
        if (status == CameraFrameSource::Status::NoFrame)
            continue;
        capturedLately++;

        if (previewThinner.takes(capturedAt)) {
            const QImage preview = previews.make(frame.get());
            if (!preview.isNull())
                emit pipeline->previewReady(preview);
        }

        const int64_t timestamp = clock.timestampOf(capturedAt);
        for (int index = 0; index < LAYERS; index++) {
            if (!encodeLayer(index, capturedAt, timestamp))
                return;
        }
    }
}

CameraPipeline::CameraPipeline(QObject *parent) : EncodePipeline(parent), worker(std::make_unique<Worker>(this)) {}

CameraPipeline::~CameraPipeline() = default;

void CameraPipeline::start(const CameraSettings &settings)
{
    worker->start(settings);
}

void CameraPipeline::stop()
{
    worker->thread.stop();
}

void CameraPipeline::requestKeyframe(int layer)
{
    if (layer >= 0 && layer < LAYERS)
        worker->keyframeWanted[static_cast<size_t>(layer)] = true;
}

void CameraPipeline::setBitrate(int bitrateBps, int layer)
{
    if (layer >= 0 && layer < LAYERS)
        worker->setBitrate(bitrateBps, layer);
}

void CameraPipeline::setEncoding(const QSize &encodeSize, int framerate, int layer)
{
    if (layer >= 0 && layer < LAYERS)
        worker->setEncoding(encodeSize, framerate, layer);
}

void CameraPipeline::setEncodingSuspended(bool suspended, int layer)
{
    if (layer >= 0 && layer < LAYERS)
        worker->suspended[static_cast<size_t>(layer)] = suspended;
}

} // namespace Video
} // namespace Core
} // namespace Acheron
