#include "Core/Video/ScreenCapture.hpp"

#if defined(Q_OS_WIN) && defined(ACHERON_HAVE_AVFILTER)

#include "Core/Logging.hpp"
#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Media/RgbScaler.hpp"
#include "Core/Video/CaptureCommon.hpp"
#include "Core/Video/CardScaler.hpp"
#include "Core/Video/PictureConverter.hpp"
#include "Core/Video/Windows/CardPictures.hpp"
#include "Core/Video/Windows/Direct3D.hpp"
#include "Core/Video/Windows/GraphicsCaptureSession.hpp"
#include "Core/Video/Windows/Monitors.hpp"
#include "Core/Video/Windows/WindowDrawer.hpp"
#include "Core/Video/Windows/WindowQueries.hpp"

#include <dwmapi.h>

#include <QByteArray>
#include <QOperatingSystemVersion>
#include <QSettings>

#include <algorithm>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

namespace Acheron {
namespace Core {
namespace Video {

using namespace Win;

namespace {

using Clock = std::chrono::steady_clock;
using Media::describeError;

// as the official client
constexpr int REACH_PAST_SCREEN = 15;
// graphics capture can hand over empty pictures for a while after a window is shown or captured
constexpr int EMPTY_PICTURES_FOR_MS = 3000;
// a screen copy has no word of a mode change, so it looks its screen up again this often
constexpr int SCREEN_RECHECK_MS = 1000;

bool rgbStaysOnCard(const ScreenCapture::Request &request)
{
    return request.keepOnCard && request.rgbOnCard;
}

QByteArray duplicationChain(const Monitor &monitor, int framerate)
{
    return "ddagrab=output_idx=" + QByteArray::number(monitor.output) + ":framerate=" + QByteArray::number(framerate);
}

bool duplicationUsable()
{
    static const bool usable = avfilter_get_by_name("ddagrab") && QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows8;
    return usable;
}

bool usesGraphicsCapture(CaptureMethod method, bool forWindow)
{
    const bool preferred = method == CaptureMethod::GraphicsCapture || (method == CaptureMethod::Auto && forWindow);
    return graphicsCaptureUsable() && (preferred || !duplicationUsable());
}

// with the desktop composited, layered windows are in the copy already, and asking for them makes the pointer flicker
DWORD screenCopyOperation()
{
    BOOL composited = FALSE;
    return SUCCEEDED(DwmIsCompositionEnabled(&composited)) && composited ? SRCCOPY : SRCCOPY | CAPTUREBLT;
}

} // namespace

struct ScreenCapture::CutOut
{
    QRect onScreen;
    QRect inPicture;

    bool operator==(const CutOut &other) const = default;
};

enum class ScreenCapture::Drawing {
    Failed,
    Unchanged,
    Drawn,
};

struct ScreenCapture::Backends
{
    ~Backends()
    {
        av_buffer_unref(&pool);
        av_buffer_unref(&device);
    }

    HWND window = nullptr;
    QSize clientSize;
    Clock::time_point sizeChangedAt;
    QSize pictureSize;
    Clock::time_point pictureDue;

    std::shared_ptr<GraphicsCaptureSession> session;
    Clock::time_point remakeAt;
    Clock::time_point emptyPicturesUntil;

    HMONITOR screenHandle = nullptr;
    std::optional<Monitor> screen;
    bool screenChanged = true;
    Clock::time_point screenCheckedAt;
    std::unique_ptr<Bitmap> screenCopy;
    PointerPainter pointer;
    bool copyDelivered = false;
    AVBufferRef *device = nullptr;
    int deviceAdapter = -1;
    AVBufferRef *pool = nullptr;
    std::shared_ptr<WindowDrawer> drawer;
    quint64 cutOutPictures = 0;
    quint64 drawnPictures = 0;
    std::optional<bool> lastWasCutOut;
    const char *drawnBecause = "";

    PictureConverter converter;
    PictureConverter rescaler;
    CardScaler scaler;
    Media::FramePointer captured{ av_frame_alloc() };
    Media::FramePointer wholeScreen{ av_frame_alloc() };
    Media::FramePointer bgra{ av_frame_alloc() };
    Media::FramePointer asDrawn{ av_frame_alloc() };

    const char *wentOn = "";
    bool skipsUnchanged = true;
    Media::FramePointer lastOfScreen{ av_frame_alloc() };
    CutOut lastCut;
    QSize drawingDelivered;
    QSize drawingInBgra;
};

bool screenCaptureAvailable()
{
    return true;
}

bool windowCaptureAvailable()
{
    return graphicsCaptureUsable() || duplicationUsable();
}

bool captureMethodUsable(CaptureMethod method)
{
    switch (method) {
    case CaptureMethod::GraphicsCapture:
        return graphicsCaptureUsable();
    case CaptureMethod::DesktopDuplication:
        return duplicationUsable();
    case CaptureMethod::ScreenCopy:
        return true;
    case CaptureMethod::Portal:
    case CaptureMethod::X11:
        return false;
    case CaptureMethod::Auto:
        break;
    }
    return true;
}

QList<CaptureSource> captureSources(CaptureMethod)
{
    QList<CaptureSource> sources;
    const std::vector<Monitor> monitors = attachedMonitors();
    for (size_t i = 0; i < monitors.size(); i++) {
        CaptureSource source;
        source.id = screenSourceId(monitors[i]);
        source.name = numberedScreenName(i + 1);
        source.geometry = monitors[i].geometry;
        sources.append(source);
    }
    return sources;
}

QList<CaptureSource> captureWindows(CaptureMethod)
{
    if (!windowCaptureAvailable())
        return {};
    return shareableWindows();
}

QImage captureThumbnail(const CaptureSource &source, const QSize &bound)
{
    if (bound.isEmpty())
        return {};

    const UnscaledCoordinates unscaled;
    if (source.kind == CaptureSource::Kind::Window)
        return windowThumbnail(windowOf(source.id), bound);
    if (source.geometry.isEmpty())
        return {};

    const HDC screen = GetDC(nullptr);
    const QImage thumbnail = shrunk(screen, source.geometry, bound);
    ReleaseDC(nullptr, screen);
    return thumbnail;
}

void restoreIfMinimised(const CaptureSource &window)
{
    const HWND handle = windowOf(window.id);
    if (handle && IsIconic(handle))
        ShowWindowAsync(handle, SW_RESTORE);
}

QSize encodeSizeOf(const CaptureSource &source, std::optional<int> maxHeight)
{
    if (source.kind == CaptureSource::Kind::Screen)
        return source.geometry.isEmpty() ? QSize() : encodeSizeFor(evenSize(source.geometry.size()), maxHeight);

    const UnscaledCoordinates unscaled;
    const HWND window = windowOf(source.id);
    const QSize inside = window && showsItself(window) ? clientOnScreen(window).size() : QSize();
    return inside.isEmpty() ? QSize() : encodeSizeFor(windowPictureSize(inside), maxHeight);
}

ScreenCapture::ScreenCapture() : backends(std::make_unique<Backends>()) {}

ScreenCapture::~ScreenCapture()
{
    tearDown();
}

void ScreenCapture::configure(const Request &wanted)
{
    startOver(wanted);
    backends = std::make_unique<Backends>();
    backends->window = windowOf(request.sourceId);
    backends->skipsUnchanged = QSettings().value(QStringLiteral("video/skip_unchanged_pictures"), true).toBool();
    // ddagrab hands a turned screen's picture over unrotated; graphics capture turns it upright
    const std::vector<Monitor> monitors = backends->window ? std::vector<Monitor>() : attachedMonitors();
    const Monitor *monitor = monitorOf(monitors, request.sourceId);
    const bool turned = monitor && monitor->turned;
    const bool copied = !backends->window && (request.method == CaptureMethod::ScreenCopy || !duplicationUsable());
    if (request.method != CaptureMethod::ScreenCopy && (usesGraphicsCapture(request.method, backends->window != nullptr) || (turned && graphicsCaptureUsable())))
        path = Path::GraphicsCapture;
    else if (copied)
        path = Path::ScreenCopy;
    else
        path = backends->window ? Path::WindowByDuplication : Path::Duplication;
    if (turned && path == Path::Duplication)
        qCWarning(LogVideo) << request.sourceId << "is turned, and without graphics capture its picture is sent the way the screen has it";
}

bool ScreenCapture::deliversOnlyChanges() const
{
    return true;
}

QString ScreenCapture::captureName() const
{
    switch (path) {
    case Path::GraphicsCapture:
        return QStringLiteral("gfxcapture");
    case Path::WindowByDuplication:
        return QStringLiteral("ddagrab %1 + PrintWindow %2 frames").arg(backends->cutOutPictures).arg(backends->drawnPictures);
    case Path::ScreenCopy:
        return QStringLiteral("BitBlt");
    case Path::Duplication:
        break;
    }
    return QStringLiteral("ddagrab");
}

ScreenCapture::Status ScreenCapture::read(AVFrame *into)
{
    if (failed)
        return Status::Failed;
    if (closed)
        return Status::Closed;

    const UnscaledCoordinates unscaled;
    if (backends->window) {
        const Status watched = watchWindow();
        if (watched != Status::Frame)
            return watched;
    }
    // ddagrab must keep being read to stay in time with the screen
    if (!picturesWanted && path != Path::Duplication) {
        std::this_thread::sleep_for(pictureInterval(request.framerate));
        return Status::NoFrame;
    }
    switch (path) {
    case Path::GraphicsCapture:
        return readGraphicsCapture(into);
    case Path::WindowByDuplication:
        return readWindowByDuplication(into);
    case Path::ScreenCopy:
        return readScreenCopy(into);
    case Path::Duplication:
        break;
    }
    return readDuplication(into);
}

ScreenCapture::Status ScreenCapture::watchWindow()
{
    Backends &state = *backends;
    if (!IsWindow(state.window)) {
        qCInfo(LogVideo) << "The window being captured was closed";
        tearDown();
        closed = true;
        return Status::Closed;
    }

    // graphics capture leaves out a window excluded from capture by itself; duplication would show it
    // black
    DWORD affinity = 0;
    const bool keptOutOfCaptures = path == Path::WindowByDuplication && GetWindowDisplayAffinity(state.window, &affinity) && affinity != 0;
    const QRect client = IsIconic(state.window) || !IsWindowVisible(state.window) || keptOutOfCaptures ? QRect() : clientOnScreen(state.window);
    if (client.isEmpty()) {
        // hiding a window ends its graphics capture; minimising it doesnt
        if (state.session && state.session->endedWith())
            state.session.reset();
        dropDuplication();
        state.clientSize = QSize();
        state.emptyPicturesUntil = Clock::now() + std::chrono::milliseconds(EMPTY_PICTURES_FOR_MS);
        failingSince.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(STALLED_WAIT_MS));
        return Status::Hidden;
    }

    const auto now = Clock::now();
    if (client.size() != state.clientSize) {
        state.clientSize = client.size();
        state.sizeChangedAt = now;
    }
    const QSize wanted = windowPictureSize(client.size());
    const bool held = now - state.sizeChangedAt >= std::chrono::milliseconds(RESIZE_SETTLE_MS);
    if (wanted != state.pictureSize && (state.pictureSize.isEmpty() || held)) {
        state.pictureSize = wanted;
        if (state.session)
            state.session->abandon();
        state.session.reset();
        av_buffer_unref(&state.pool);
        av_frame_unref(state.lastOfScreen.get());
        state.drawingDelivered = QSize();
    }
    return Status::Frame;
}

bool ScreenCapture::scaleOnCard(const AVFrame *from, const QRect &area, const QSize &size, const QRect &placed, AVFrame *into)
{
    const CardScaler::Output output = { size, Media::encodesAsBt601(size.height(), request.bt601Only), request.rgbOnCard };
    if (backends->scaler.scale(from, area, output, placed, into))
        return true;
    suspendCardScaling();
    return false;
}

ScreenCapture::Status ScreenCapture::readDuplication(AVFrame *into)
{
    if (!graph) {
        if (waitedForRetry(retryAt))
            return Status::Stalled;
        const int built = buildDuplication();
        if (built < 0)
            return afterFailure(built);
    }

    // read even when no picture is wanted: ddagrab makes up a missed read by handing the next pictures over
    // at once
    Backends &state = *backends;
    AVFrame *captured = state.captured.get();
    const int got = av_buffersink_get_frame(sink, captured);
    if (got == AVERROR(EAGAIN))
        return Status::NoFrame;
    if (got < 0)
        return afterFailure(got);

    if (!picturesWanted || (state.skipsUnchanged && isSamePictureOnCard(state.lastOfScreen.get(), captured))) {
        av_frame_unref(captured);
        return Status::NoFrame;
    }
    keepReferenceTo(state.lastOfScreen.get(), captured);
    return deliverFromCard(captured, into);
}

ScreenCapture::Status ScreenCapture::readGraphicsCapture(AVFrame *into)
{
    Backends &state = *backends;
    if (!state.session) {
        if (waitedForRetry(retryAt))
            return Status::Stalled;
        if (waitedForRetry(state.remakeAt))
            return Status::NoFrame;
        const int built = buildGraphicsCapture();
        if (built < 0 && path != Path::GraphicsCapture)
            return Status::Stalled;
        if (built < 0)
            return afterFailure(built);
    }

    const auto interval = pictureInterval(request.framerate);
    const auto giveUpAt = std::max(Clock::now(), state.pictureDue) + std::chrono::milliseconds(NOTHING_NEW_WAIT_MS);
    // a picture a little early is the one a source at twice the rate has ready; the next would be late
    const GraphicsCaptureSession::Taken taken = state.session->take(state.captured.get(), state.pictureDue - interval / 4, giveUpAt);
    if (taken == GraphicsCaptureSession::Taken::Ended) {
        const int ended = *state.session->endedWith();
        if (ended != AVERROR_EOF || !state.window)
            return afterFailure(ended);
        state.session.reset();
        state.remakeAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
        return Status::NoFrame;
    }
    if (taken == GraphicsCaptureSession::Taken::Nothing)
        return Status::NoFrame;
    state.pictureDue = std::max(state.pictureDue + interval, Clock::now() + interval / 2);
    // graphics capture hands over a fully transparent picture as a window is hidden, shown or closed, and
    // old pictures wait in it, so pictures are checked until the first one with content
    bool undrawn = state.window && !showsItself(state.window);
    if (state.window && !undrawn && Clock::now() < state.emptyPicturesUntil) {
        undrawn = isEmptyPicture(state.captured.get(), state.bgra.get());
        if (!undrawn) {
            state.emptyPicturesUntil = {};
            av_frame_unref(state.bgra.get());
        }
    }
    if (undrawn) {
        av_frame_unref(state.captured.get());
        return Status::NoFrame;
    }

    return deliverFromCard(state.captured.get(), into);
}

ScreenCapture::Status ScreenCapture::deliverFromCard(AVFrame *captured, AVFrame *into)
{
    Backends &state = *backends;
    const QSize ownSize(captured->width, captured->height);
    const QSize size = encodeSizeFor(ownSize, request.maxHeight);
    const char *wentOn = "read back and scaled in software";
    const bool bt601 = Media::encodesAsBt601(size.height(), request.bt601Only);
    if (rgbStaysOnCard(request) && size == ownSize) {
        wentOn = "kept on the graphics card as it is";
        av_frame_move_ref(into, captured);
        markColours(into, false);
    } else if (scalesOnCard() && scaleOnCard(captured, wholeOf(captured), size, QRect(QPoint(0, 0), size), into)) {
        wentOn = "scaled on the graphics card and kept there";
        av_frame_unref(captured);
    } else {
        int converted = readBack(captured, state.bgra.get());
        av_frame_unref(captured);
        if (converted >= 0)
            converted = state.converter.convert(state.bgra.get(), size, request.softwareFormat, bt601, into);
        if (converted < 0)
            return afterFailure(converted);
        markColours(into, bt601);
    }

    if (path == Path::Duplication && std::exchange(state.wentOn, wentOn) != wentOn)
        qCInfo(LogVideo) << "Capturing" << request.sourceId << "as" << size.width() << "x" << size.height() << wentOn;
    delivered();
    return Status::Frame;
}

// the official client's way with a window on Windows 10
ScreenCapture::Status ScreenCapture::readWindowByDuplication(AVFrame *into)
{
    Backends &state = *backends;
    std::this_thread::sleep_until(state.pictureDue);
    state.pictureDue = std::max(state.pictureDue + pictureInterval(request.framerate), Clock::now());

    const QRect client = clientOnScreen(state.window);
    const bool screenDuplicated = duplicateScreenOf(client);
    const QSize size = encodeSizeFor(state.pictureSize, request.maxHeight);
    const bool asCaptured = state.pool && rgbStaysOnCard(request) && size == state.pictureSize;
    const bool scaledOnCard = state.pool && !asCaptured && scalesOnCard();
    const bool staysOnCard = asCaptured || scaledOnCard;

    AVFrame *whole = state.wholeScreen.get();
    AVFrame *card = state.captured.get();
    AVFrame *bgra = state.bgra.get();
    CutOut cut;
    state.drawnBecause = "its screen cannot be duplicated";
    const bool cutOut = screenDuplicated && cutOutOfScreen(client, &cut);
    if (cutOut && state.skipsUnchanged && isSamePictureOnCard(state.lastOfScreen.get(), whole) && cut == state.lastCut) {
        av_frame_unref(whole);
        return Status::NoFrame;
    }
    if (!cutOut && drawnByItself(client) != Drawing::Drawn)
        return Status::NoFrame;

    bool ready = true;
    if (cutOut && scaledOnCard) {
        ready = scaleOnCard(whole, cut.onScreen, size, scaledPart(cut.inPicture, state.pictureSize, size), into);
    } else {
        if (cutOut)
            ready = cutOntoCard(whole, cut.onScreen, state.pool, cut.inPicture.topLeft(), card) && (staysOnCard || (prepareBgra(bgra, state.pictureSize) && av_hwframe_transfer_data(bgra, card, 0) >= 0));
        else if (staysOnCard)
            ready = av_hwframe_get_buffer(state.pool, card, 0) >= 0 && av_hwframe_transfer_data(card, bgra, 0) >= 0;

        const bool bt601 = !staysOnCard && Media::encodesAsBt601(size.height(), request.bt601Only);
        if (ready && scaledOnCard)
            ready = scaleOnCard(card, wholeOf(card), size, QRect(QPoint(0, 0), size), into);
        else if (ready && staysOnCard)
            av_frame_move_ref(into, card);
        else if (ready)
            ready = state.converter.convert(bgra, size, request.softwareFormat, bt601, into) >= 0;
        if (ready && !scaledOnCard)
            markColours(into, bt601);
    }
    av_frame_unref(card);
    av_frame_unref(state.lastOfScreen.get());
    if (ready && cutOut) {
        av_frame_move_ref(state.lastOfScreen.get(), whole);
        state.lastCut = cut;
    }
    state.drawingDelivered = ready && !cutOut ? state.drawingInBgra : QSize();
    av_frame_unref(whole);
    if (!ready)
        return Status::NoFrame;

    if (state.lastWasCutOut != cutOut) {
        state.lastWasCutOut = cutOut;
        qCInfo(LogVideo) << "Capturing" << request.sourceId << "as" << size.width() << "x" << size.height() << (staysOnCard ? "on the graphics card," : "scaled in software,") << (cutOut ? "cut out of its screen" : "drawn by itself, as") << (cutOut ? "" : state.drawnBecause);
    }
    (cutOut ? state.cutOutPictures : state.drawnPictures)++;
    delivered();
    return Status::Frame;
}

ScreenCapture::Status ScreenCapture::readScreenCopy(AVFrame *into)
{
    Backends &state = *backends;
    if (!state.screen && waitedForRetry(retryAt))
        return Status::Stalled;
    std::this_thread::sleep_until(state.pictureDue);
    state.pictureDue = std::max(state.pictureDue + pictureInterval(request.framerate), Clock::now());

    const auto now = Clock::now();
    if (!state.screen || now - state.screenCheckedAt >= std::chrono::milliseconds(SCREEN_RECHECK_MS)) {
        const std::vector<Monitor> monitors = attachedMonitors();
        const Monitor *monitor = monitorOf(monitors, request.sourceId);
        if (!monitor)
            return afterFailure(AVERROR(ENODEV));
        state.screen = *monitor;
        state.screenCheckedAt = now;
    }

    const QRect area = state.screen->geometry;
    if (!state.screenCopy || state.screenCopy->size() != area.size()) {
        state.screenCopy = std::make_unique<Bitmap>(area.size());
        state.copyDelivered = false;
    }
    if (!state.screenCopy->usable())
        return afterFailure(AVERROR(ENOMEM));

    const HDC screen = GetDC(nullptr);
    const bool copied = screen && BitBlt(state.screenCopy->context(), 0, 0, area.width(), area.height(), screen, area.x(), area.y(), screenCopyOperation());
    if (screen)
        ReleaseDC(nullptr, screen);
    // as with duplication, the secure desktop (a UAC prompt, the lock screen) refuses to be copied
    if (!copied) {
        state.screen.reset();
        return afterFailure(AVERROR(EPERM));
    }
    state.pointer.paint(state.screenCopy->context(), area.size(), area);

    AVFrame *bgra = state.bgra.get();
    const QSize pictureSize = evenSize(area.size());
    if (state.skipsUnchanged && state.copyDelivered && bgra->width == pictureSize.width() && showsTopLeftOf(*state.screenCopy, bgra))
        return Status::NoFrame;
    if (!prepareBgra(bgra, pictureSize))
        return afterFailure(AVERROR(ENOMEM));
    copyTopLeft(*state.screenCopy, bgra);

    const QSize size = encodeSizeFor(pictureSize, request.maxHeight);
    const bool bt601 = Media::encodesAsBt601(size.height(), request.bt601Only);
    const int converted = state.converter.convert(bgra, size, request.softwareFormat, bt601, into);
    state.copyDelivered = converted >= 0;
    if (converted < 0)
        return afterFailure(converted);
    markColours(into, bt601);

    const char *const wentOn = "copied by GDI and scaled in software";
    if (std::exchange(state.wentOn, wentOn) != wentOn)
        qCInfo(LogVideo) << "Capturing" << request.sourceId << "as" << size.width() << "x" << size.height() << wentOn;
    delivered();
    return Status::Frame;
}

bool ScreenCapture::duplicateScreenOf(const QRect &client)
{
    Backends &state = *backends;
    const QPoint middle = client.center();
    const HMONITOR under = MonitorFromPoint({ middle.x(), middle.y() }, MONITOR_DEFAULTTONULL);
    const bool retryDue = Clock::now() >= retryAt;
    if (under != state.screenHandle || (state.screenChanged && retryDue)) {
        dropDuplication();
        state.screenHandle = under;
        state.screenChanged = false;
        state.screen.reset();
        for (const Monitor &each : attachedMonitors()) {
            if (each.handle == under)
                state.screen = each;
        }
    }

    const int adapter = state.screen ? state.screen->adapter : std::max(0, state.deviceAdapter);
    if (adapter != state.deviceAdapter || (!state.device && retryDue)) {
        dropDuplication();
        av_buffer_unref(&state.pool);
        av_buffer_unref(&state.device);
        state.deviceAdapter = adapter;
        if (openDirect3dDevice(&state.device, QByteArray::number(adapter)) < 0)
            retryLater();
    }
    if (state.device && !state.pool)
        state.pool = cardPictures(state.device, state.pictureSize);

    if (!state.pool || !state.screen || state.screen->turned)
        return false;
    if (graph)
        return true;
    if (!retryDue)
        return false;

    const int built = assemble(duplicationChain(*state.screen, request.framerate), state.device, &graph, &sink);
    if (built < 0) {
        // Windows takes the screen away for a mode change, and it may come back another size
        state.screenChanged = true;
        retryLater();
    }
    return built >= 0;
}

bool ScreenCapture::cutOutOfScreen(const QRect &client, CutOut *cut)
{
    Backends &state = *backends;
    state.drawnBecause = "its size is changing, or is smaller than what is sent";
    if (evenSize(client.size()) != state.pictureSize)
        return false;
    state.drawnBecause = "a window above overlaps it";
    if (isCoveredOnScreen(state.window, client, ShellSurfaces::LeftForLater))
        return false;

    state.drawnBecause = "it reaches past its screen";
    const QRect screen = state.screen->geometry;
    if (!screen.adjusted(-REACH_PAST_SCREEN, -REACH_PAST_SCREEN, REACH_PAST_SCREEN, REACH_PAST_SCREEN).contains(client))
        return false;
    const QRect onScreen = client & screen;
    const QRect inPicture = QRect(onScreen.topLeft() - client.topLeft(), onScreen.size()) & QRect(QPoint(0, 0), state.pictureSize);

    state.drawnBecause = "its screen had no picture";
    AVFrame *whole = state.wholeScreen.get();
    av_frame_unref(whole);
    const int got = av_buffersink_get_frame(sink, whole);
    if (got < 0) {
        if (got != AVERROR(EAGAIN)) {
            dropDuplication();
            state.screenChanged = true;
            retryLater();
        }
        return false;
    }

    state.drawnBecause = "it moved or was covered while its screen was read";
    if (clientOnScreen(state.window) != client || isCoveredOnScreen(state.window, client, ShellSurfaces::LookedFor)) {
        av_frame_unref(whole);
        return false;
    }
    *cut = { inPicture.translated(client.topLeft() - screen.topLeft()), inPicture };
    return true;
}

ScreenCapture::Drawing ScreenCapture::drawnByItself(const QRect &client)
{
    Backends &state = *backends;
    if (!state.drawer)
        state.drawer = WindowDrawer::start(state.window, true);
    if (!state.drawer->draw(std::chrono::milliseconds(DRAWING_LIMIT_MS), stillWanted))
        return Drawing::Failed;
    state.drawer->drawPointer(client);

    const Bitmap &drawn = state.drawer->drawn();
    AVFrame *bgra = state.bgra.get();
    const QSize delivered = std::exchange(state.drawingDelivered, QSize());
    state.drawingInBgra = QSize();
    if (drawn.size() == client.size()) {
        if (!prepareBgra(bgra, state.pictureSize))
            return Drawing::Failed;
        if (state.skipsUnchanged && delivered == drawn.size() && showsTopLeftOf(drawn, bgra)) {
            state.drawingDelivered = delivered;
            return Drawing::Unchanged;
        }
        copyTopLeft(drawn, bgra);
        state.drawingInBgra = drawn.size();
        return Drawing::Drawn;
    }

    // a program Windows scales for the screen draws itself smaller than it shows
    AVFrame *asDrawn = state.asDrawn.get();
    if (!prepareBgra(asDrawn, drawn.size()))
        return Drawing::Failed;
    copyTopLeft(drawn, asDrawn);
    av_frame_unref(bgra);
    return state.rescaler.convert(asDrawn, state.pictureSize, AV_PIX_FMT_BGRA, false, bgra) >= 0 ? Drawing::Drawn : Drawing::Failed;
}

// neither source recovers by itself: ddagrab not from losing its screen or a mode change, graphics
// capture not from its window being hidden
ScreenCapture::Status ScreenCapture::afterFailure(int error)
{
    tearDown();
    const QString reason = captureLostText(backends->window ? CaptureSource::Kind::Window : CaptureSource::Kind::Screen);
    // access is denied while the secure desktop shows (a UAC prompt, the lock screen)
    const bool secureDesktopShowing = error == AVERROR(EPERM) && everDelivered;
    return giveUpOrRetry(reason.arg(describeError(error)), secureDesktopShowing);
}

int ScreenCapture::buildDuplication()
{
    const std::vector<Monitor> monitors = attachedMonitors();
    const Monitor *monitor = monitorOf(monitors, request.sourceId);
    if (!monitor)
        return AVERROR(ENODEV);

    // ddagrab counts outputs on the adapter of the device it is given
    AVBufferRef *device = nullptr;
    int result = openDirect3dDevice(&device, QByteArray::number(monitor->adapter));
    if (result < 0)
        return result;

    result = assemble(duplicationChain(*monitor, request.framerate), device, &graph, &sink);
    av_buffer_unref(&device);
    backends->wentOn = "";
    return result;
}

int ScreenCapture::buildGraphicsCapture()
{
    Backends &state = *backends;

    QSize ownSize = state.pictureSize;
    QByteArray source;
    QByteArray adapter;
    if (state.window) {
        source = ":hwnd=" + QByteArray::number(static_cast<qulonglong>(reinterpret_cast<quintptr>(state.window)));
    } else {
        const std::vector<Monitor> monitors = attachedMonitors();
        const Monitor *monitor = monitorOf(monitors, request.sourceId);
        if (!monitor)
            return AVERROR(ENODEV);
        ownSize = evenSize(monitor->geometry.size());
        source = ":hmonitor=" + QByteArray::number(static_cast<qulonglong>(reinterpret_cast<quintptr>(monitor->handle)));
        adapter = QByteArray::number(monitor->adapter);
    }

    // graphics capture stretches what it captured over the whole picture, which is wrong for a window
    // smaller than the smallest picture
    const QSize size = encodeSizeFor(ownSize, request.maxHeight);
    const bool asCaptured = rgbStaysOnCard(request) && size == ownSize;
    const bool fillsItsPicture = !state.window || evenSize(state.clientSize) == ownSize;
    const bool scalesByItself = rgbStaysOnCard(request) && size != ownSize && fillsItsPicture;
    const QSize captured = scalesByItself ? size : ownSize;

    // otherwise the filter fixes a size before the first picture from what Windows says the window
    // measures, which isnt always what it hands over
    QByteArray chain = "gfxcapture=max_framerate=" + QByteArray::number(request.framerate) + source + ":width=" + QByteArray::number(captured.width()) + ":height=" + QByteArray::number(captured.height());
    if (scalesByItself)
        chain += ":resize_mode=scale";
    int result = 0;
    state.session = GraphicsCaptureSession::start(chain, adapter, &result);
    if (state.window)
        state.emptyPicturesUntil = Clock::now() + std::chrono::milliseconds(EMPTY_PICTURES_FOR_MS);
    if (result >= 0) {
        qCInfo(LogVideo) << "Capturing" << request.sourceId << "by graphics capture as" << size.width() << "x" << size.height() << (asCaptured ? "kept on the graphics card as it is" : scalesByItself ? "scaled by graphics capture and kept on the graphics card"
                                                                                                                                                                                : scalesOnCard()       ? "scaled on the graphics card and kept there"
                                                                                                                                                                                                       : "read back and scaled in software");
        return result;
    }

    if (result == AVERROR(ENOSYS))
        ruleOutGraphicsCapture();
    const bool leftToChoose = request.method == CaptureMethod::Auto && state.window && !everDelivered;
    if ((result == AVERROR(ENOSYS) || leftToChoose) && duplicationUsable()) {
        qCInfo(LogVideo) << "Graphics capture of" << request.sourceId << "failed (" << describeError(result) << "); using desktop duplication";
        path = state.window ? Path::WindowByDuplication : Path::Duplication;
    } else if (result == AVERROR(ENOSYS) && !state.window) {
        qCInfo(LogVideo) << "Graphics capture of" << request.sourceId << "failed (" << describeError(result) << "); copying the screen";
        path = Path::ScreenCopy;
    }
    return result;
}

void ScreenCapture::dropDuplication()
{
    avfilter_graph_free(&graph);
    sink = nullptr;
    av_frame_unref(backends->lastOfScreen.get());
}

void ScreenCapture::tearDown()
{
    dropDuplication();
    if (backends->session)
        backends->session->abandon();
    backends->session.reset();
    if (backends->drawer)
        backends->drawer->abandon();
    backends->drawer.reset();
}

} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
