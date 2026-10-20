#include "Core/Video/ScreenCapture.hpp"

#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && defined(ACHERON_HAVE_AVFILTER)

#include "Core/Logging.hpp"
#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Media/HardwareDevices.hpp"
#include "Core/Media/RgbScaler.hpp"
#include "Core/Video/CaptureCommon.hpp"
#include "Core/Video/CardScaler.hpp"
#include "Core/Video/LinuxCapture.hpp"
#include "Core/Video/PictureConverter.hpp"

#include <QSettings>

#include <algorithm>
#include <cstring>
#include <thread>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/macros.h>
}

namespace Acheron {
namespace Core {
namespace Video {

namespace {

using Clock = CaptureClock;

const QLatin1String PORTAL_SCREEN_ID("portal:screen");
const QLatin1String PORTAL_WINDOW_ID("portal:window");
const QLatin1String PORTAL_SCREEN_AGAIN_ID("portal:screen:again");
const QLatin1String PORTAL_WINDOW_AGAIN_ID("portal:window:again");
const char *const PORTAL_SCREEN_TOKEN_KEY = "video/portal_screen_token";
const char *const PORTAL_WINDOW_TOKEN_KEY = "video/portal_window_token";
const QLatin1String PIPEWIRE_ID_PREFIX("pipewire:");
const QLatin1String WINDOW_ID_PREFIX("window:");

using Media::describeError;
using Media::FramePointer;

bool isFromPipeWire(const QString &sourceId)
{
    return sourceId.startsWith(PIPEWIRE_ID_PREFIX);
}

bool usesPortal(CaptureMethod method)
{
    return PortalCapture::usable() && (method == CaptureMethod::Portal || !X11Capture::usable());
}

CaptureSource chosenInTheDesktopsDialog(CaptureSource::Kind kind)
{
    CaptureSource choice;
    choice.kind = kind;
    choice.id = kind == CaptureSource::Kind::Window ? PORTAL_WINDOW_ID : PORTAL_SCREEN_ID;
    choice.name = kind == CaptureSource::Kind::Window ? captureText("Window") : captureText("Screen");
    return choice;
}

const char *tokenKeyOf(CaptureSource::Kind kind)
{
    return kind == CaptureSource::Kind::Window ? PORTAL_WINDOW_TOKEN_KEY : PORTAL_SCREEN_TOKEN_KEY;
}

bool standsForTheLastChoice(const CaptureSource &source)
{
    return source.id == PORTAL_SCREEN_AGAIN_ID || source.id == PORTAL_WINDOW_AGAIN_ID;
}

// A new choice in the desktop's dialog and, once the desktop has handed out a token to remember
// one by, the same again without the dialog.
QList<CaptureSource> offeredByTheDesktop(CaptureSource::Kind kind)
{
    QList<CaptureSource> offered = { chosenInTheDesktopsDialog(kind) };
    if (QSettings().value(tokenKeyOf(kind)).toString().isEmpty())
        return offered;

    CaptureSource again = offered.first();
    again.id = kind == CaptureSource::Kind::Window ? PORTAL_WINDOW_AGAIN_ID : PORTAL_SCREEN_AGAIN_ID;
    again.name = kind == CaptureSource::Kind::Window ? captureText("Window shared last time") : captureText("Screen shared last time");
    offered.append(again);
    return offered;
}

AVPixelFormat formatOfFourcc(uint32_t fourcc)
{
    switch (fourcc) {
    case MKTAG('X', 'R', '2', '4'):
        return AV_PIX_FMT_BGR0;
    case MKTAG('A', 'R', '2', '4'):
        return AV_PIX_FMT_BGRA;
    case MKTAG('X', 'B', '2', '4'):
        return AV_PIX_FMT_RGB0;
    case MKTAG('A', 'B', '2', '4'):
        return AV_PIX_FMT_RGBA;
    }
    return AV_PIX_FMT_NONE;
}

// The top left of `from` in a black picture of `size`.
bool placedOnBlack(const AVFrame *from, const QSize &size, AVFrame *into)
{
    av_frame_unref(into);
    into->format = from->format;
    into->width = size.width();
    into->height = size.height();
    if (av_frame_get_buffer(into, 0) < 0)
        return false;

    const int columns = std::min(from->width, into->width);
    for (int y = 0; y < into->height; y++) {
        uint8_t *to = into->data[0] + static_cast<qsizetype>(y) * into->linesize[0];
        const int copied = y < from->height ? columns : 0;
        if (copied > 0)
            memcpy(to, from->data[0] + static_cast<qsizetype>(y) * from->linesize[0], static_cast<size_t>(copied) * 4);
        memset(to + static_cast<qsizetype>(copied) * 4, 0, static_cast<size_t>(into->width - copied) * 4);
    }
    return true;
}

} // namespace

struct ScreenCapture::Backends
{
    ~Backends()
    {
        av_buffer_unref(&mappedPictures);
        av_buffer_unref(&device);
    }

    bool mapOntoCard(const AVFrame *prime);

    std::unique_ptr<LinuxGrabber> grabber;
    bool grabbedOnCard = false;
    Clock::time_point openedAt;
    QSize announced;
    QString lastFailure;

    PictureConverter converter;
    CardScaler scaler;
    AVBufferRef *device = nullptr;
    AVBufferRef *mappedPictures = nullptr;
    FramePointer captured{ av_frame_alloc() };
    FramePointer onBlack{ av_frame_alloc() };
    FramePointer mapped{ av_frame_alloc() };
};

// A picture the desktop left on the card, as one the card's scaler takes.
bool ScreenCapture::Backends::mapOntoCard(const AVFrame *prime)
{
    const auto *layout = reinterpret_cast<const AVDRMFrameDescriptor *>(prime->data[0]);
    const AVPixelFormat format = layout && layout->nb_layers > 0 ? formatOfFourcc(layout->layers[0].format) : AV_PIX_FMT_NONE;
    if (format == AV_PIX_FMT_NONE)
        return false;
    if (!device && (!Media::hardwareDeviceCanBeAskedFor(AV_HWDEVICE_TYPE_VAAPI) || av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0))
        return false;

    auto *pictures = mappedPictures ? reinterpret_cast<AVHWFramesContext *>(mappedPictures->data) : nullptr;
    if (!pictures || pictures->sw_format != format || pictures->width != prime->width || pictures->height != prime->height) {
        av_buffer_unref(&mappedPictures);
        mappedPictures = av_hwframe_ctx_alloc(device);
        if (!mappedPictures)
            return false;
        pictures = reinterpret_cast<AVHWFramesContext *>(mappedPictures->data);
        pictures->format = AV_PIX_FMT_VAAPI;
        pictures->sw_format = format;
        pictures->width = prime->width;
        pictures->height = prime->height;
        if (av_hwframe_ctx_init(mappedPictures) < 0) {
            av_buffer_unref(&mappedPictures);
            return false;
        }
    }

    av_frame_unref(mapped.get());
    mapped->format = AV_PIX_FMT_VAAPI;
    mapped->hw_frames_ctx = av_buffer_ref(mappedPictures);
    if (!mapped->hw_frames_ctx || av_hwframe_map(mapped.get(), prime, AV_HWFRAME_MAP_READ) < 0)
        return false;
    av_frame_copy_props(mapped.get(), prime);
    return true;
}

bool screenCaptureAvailable()
{
    return X11Capture::usable() || PortalCapture::usable();
}

bool windowCaptureAvailable()
{
    return screenCaptureAvailable();
}

bool captureMethodUsable(CaptureMethod method)
{
    switch (method) {
    case CaptureMethod::Portal:
        return PortalCapture::usable();
    case CaptureMethod::X11:
        return X11Capture::usable();
    case CaptureMethod::GraphicsCapture:
    case CaptureMethod::DesktopDuplication:
        return false;
    case CaptureMethod::Auto:
        break;
    }
    return screenCaptureAvailable();
}

QList<CaptureSource> captureSources(CaptureMethod method)
{
    if (usesPortal(method))
        return offeredByTheDesktop(CaptureSource::Kind::Screen);
    return X11Capture::screens();
}

QList<CaptureSource> captureWindows(CaptureMethod method)
{
    if (usesPortal(method))
        return offeredByTheDesktop(CaptureSource::Kind::Window);
    return X11Capture::windows();
}

bool isChosenByTheDesktop(const CaptureSource &source)
{
    return source.id == PORTAL_SCREEN_ID || source.id == PORTAL_WINDOW_ID || standsForTheLastChoice(source);
}

bool isHandedOverByTheDesktop(const CaptureSource &source)
{
    return isFromPipeWire(source.id);
}

void chooseSource(const CaptureSource &source, QObject *receiver, std::function<void(const CaptureSource &, const QString &)> chosen)
{
    if (!isChosenByTheDesktop(source)) {
        chosen(source, QString());
        return;
    }

    // A token is good for one use: the desktop hands out the next with what it hands over, and
    // PortalCapture has put that in the old one's place by the time it reports. A desktop that
    // no longer knows a token shows its dialog after all.
    static QString tokens[2];
    QString *token = &tokens[source.kind == CaptureSource::Kind::Window ? 1 : 0];
    const char *key = tokenKeyOf(source.kind);
    *token = standsForTheLastChoice(source) ? QSettings().value(key).toString() : QString();
    PortalCapture::chooseAgain(source.kind, token, receiver, [token, key, chosen = std::move(chosen)](const CaptureSource &handedOver, const QString &failure) {
        if (!handedOver.id.isEmpty())
            QSettings().setValue(key, *token);
        chosen(handedOver, failure);
    });
}

void releaseSource(const CaptureSource &source)
{
    if (isFromPipeWire(source.id))
        PortalCapture::release(source.id);
}

QImage captureThumbnail(const CaptureSource &source, const QSize &bound)
{
    if (bound.isEmpty() || isChosenByTheDesktop(source) || isFromPipeWire(source.id))
        return {};
    return X11Capture::thumbnail(source, bound);
}

void restoreIfMinimised(const CaptureSource &window)
{
    if (window.id.startsWith(WINDOW_ID_PREFIX))
        X11Capture::restoreIfMinimised(window);
}

QSize encodeSizeOf(const CaptureSource &source, int maxHeight)
{
    if (isChosenByTheDesktop(source))
        return {};
    const QSize own = isFromPipeWire(source.id) ? PortalCapture::sizeOf(source) : X11Capture::sizeOf(source);
    return own.isEmpty() ? QSize() : encodeSizeFor(windowPictureSize(own), maxHeight);
}

ScreenCapture::ScreenCapture() : backends(std::make_unique<Backends>()) {}

ScreenCapture::~ScreenCapture() = default;

void ScreenCapture::configure(const Request &wanted)
{
    request = wanted;
    failingSince.reset();
    retryAt = {};
    closed = false;
    failed = false;
    cardScalingResumesAt.reset();
    failureText.clear();
    backends = std::make_unique<Backends>();
}

bool ScreenCapture::deliversOnlyChanges() const
{
    return backends->grabber ? backends->grabber->deliversOnlyChanges() : isFromPipeWire(request.sourceId);
}

QString ScreenCapture::captureName() const
{
    if (backends->grabber)
        return backends->grabber->name();
    return isFromPipeWire(request.sourceId) ? QStringLiteral("pipewire") : QStringLiteral("x11grab");
}

void ScreenCapture::delivered()
{
    everDelivered = true;
    failingSince.reset();
}

void ScreenCapture::tearDown()
{
    backends->grabber.reset();
}

ScreenCapture::Status ScreenCapture::afterFailure(int error)
{
    const QString reason = backends->lastFailure.isEmpty() ? captureText("The screen can no longer be captured (%1).").arg(describeError(error)) : backends->lastFailure;
    backends->lastFailure.clear();
    tearDown();
    return giveUpOrRetry(reason, false);
}

ScreenCapture::Status ScreenCapture::read(AVFrame *into)
{
    if (failed)
        return Status::Failed;
    if (closed)
        return Status::Closed;

    Backends &state = *backends;
    if (!state.grabber) {
        if (waitedForRetry(retryAt))
            return Status::Stalled;
        state.grabbedOnCard = request.keepOnCard && !cardScalingSuspended() && CardScaler::usable();
        state.grabber = isFromPipeWire(request.sourceId) ? PortalCapture::open(request.sourceId, request.framerate, state.grabbedOnCard) : X11Capture::open(request.sourceId, request.framerate);
        state.announced = QSize();
        state.openedAt = Clock::now();
        if (!state.grabber)
            return afterFailure(AVERROR(ENODEV));
    }

    AVFrame *captured = state.captured.get();
    av_frame_unref(captured);
    const auto wait = std::chrono::milliseconds(1000 / std::max(1, request.framerate) + NOTHING_NEW_WAIT_MS);
    switch (state.grabber->grab(captured, wait)) {
    case LinuxGrabber::Result::Frame:
        break;
    case LinuxGrabber::Result::NoFrame:
        // Twice as long as a capture that never comes up takes to say so.
        if (failingSince && Clock::now() - state.openedAt >= std::chrono::milliseconds(2 * GIVE_UP_AFTER_MS))
            failingSince.reset();
        return Status::NoFrame;
    case LinuxGrabber::Result::Hidden:
        failingSince.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(STALLED_WAIT_MS));
        return Status::Hidden;
    case LinuxGrabber::Result::Closed:
        qCInfo(LogVideo) << "The source being captured was closed";
        tearDown();
        closed = true;
        return Status::Closed;
    case LinuxGrabber::Result::Failed:
        state.lastFailure = state.grabber->failure();
        return afterFailure(AVERROR(EIO));
    }

    // The source is still read, so that its pictures do not pile up, and found to be there.
    if (!picturesWanted) {
        av_frame_unref(captured);
        delivered();
        return Status::NoFrame;
    }

    const bool onCard = captured->format == AV_PIX_FMT_DRM_PRIME;
    if (!onCard)
        av_frame_apply_cropping(captured, AV_FRAME_CROP_UNALIGNED);
    const QSize ownSize(captured->width - static_cast<int>(captured->crop_left + captured->crop_right), captured->height - static_cast<int>(captured->crop_top + captured->crop_bottom));
    const QSize pictureSize = windowPictureSize(ownSize);
    const QSize size = encodeSizeFor(pictureSize, request.maxHeight);
    const bool bt601 = request.bt601Only || !Media::usesBt709AtHeight(size.height());
    const bool fillsItsPicture = ownSize.width() >= pictureSize.width() && ownSize.height() >= pictureSize.height();

    if (std::exchange(state.announced, size) != size)
        qCInfo(LogVideo) << "Capturing" << request.sourceId << "by" << state.grabber->name() << "as" << size.width() << "x" << size.height() << (onCard ? "scaled on the graphics card and kept there" : "scaled in software");

    av_frame_unref(into);
    if (onCard) {
        const bool scaled = fillsItsPicture && state.mapOntoCard(captured) && state.scaler.scale(state.mapped.get(), CardScaler::Output{ size, bt601, false }, into);
        av_frame_unref(state.mapped.get());
        av_frame_unref(captured);
        if (!scaled) {
            if (fillsItsPicture)
                qCInfo(LogVideo) << "The graphics card would not take or scale the desktop's pictures of" << request.sourceId << "; capturing them into memory for a while";
            else
                qCInfo(LogVideo) << request.sourceId << "is smaller than is sent, which only memory can make up; capturing it into memory for a while";
            suspendCardScaling();
            tearDown();
            return Status::NoFrame;
        }
        delivered();
        return Status::Frame;
    }

    AVFrame *picture = captured;
    if (fillsItsPicture) {
        captured->width = pictureSize.width();
        captured->height = pictureSize.height();
    } else if (placedOnBlack(captured, pictureSize, state.onBlack.get())) {
        picture = state.onBlack.get();
    } else {
        return afterFailure(AVERROR(ENOMEM));
    }

    const int converted = state.converter.convert(picture, size, request.softwareFormat, bt601, into);
    av_frame_unref(captured);
    if (converted < 0)
        return afterFailure(converted);
    markColours(into, bt601);
    delivered();
    return Status::Frame;
}

} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
