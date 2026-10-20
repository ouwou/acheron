#include "Core/Video/ScreenCapture.hpp"

// Capture is ffmpeg's desktop duplication and graphics capture sources, which are in libavfilter.
#if defined(Q_OS_WIN) && defined(ACHERON_HAVE_AVFILTER)

#include "Core/Logging.hpp"
#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Media/RgbScaler.hpp"
#include "Core/Video/CaptureCommon.hpp"
#include "Core/Video/CardScaler.hpp"
#include "Core/Video/PictureConverter.hpp"
#include "Core/Video/Windows/CardPictures.hpp"
#include "Core/Video/Windows/GraphicsCaptureSession.hpp"
#include "Core/Video/Windows/Monitors.hpp"
#include "Core/Video/Windows/WindowDrawer.hpp"
#include "Core/Video/Windows/WindowQueries.hpp"

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

using Clock = CaptureClock;
using Media::describeError;

// How far a window may reach past the edge of its screen and still be cut out of it, as the
// official client has it.
constexpr int REACH_PAST_SCREEN = 15;
// How long after a window was last seen hidden, or its capture begun, its pictures are read
// back and looked at, unless one with something in it comes sooner. Only a window that has
// nothing to show wherever it is looked at is held up for all of it.
constexpr int EMPTY_PICTURES_FOR_MS = 3000;

bool usesGraphicsCapture(CaptureMethod method, bool forWindow)
{
    const bool preferred = method == CaptureMethod::GraphicsCapture || (method == CaptureMethod::Auto && forWindow);
    return graphicsCaptureUsable() && (preferred || !screenCaptureAvailable());
}

} // namespace

// The part of its screen's picture that shows a window, and where in the window's own picture
// that part belongs. They differ in size only where the window reaches past its screen.
struct ScreenCapture::CutOut
{
    QRect onScreen;
    QRect inPicture;

    bool operator==(const CutOut &other) const = default;
};

enum class ScreenCapture::Drawing {
    Failed,
    // What the window drew is what the last picture, which it also drew, shows.
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
    // What is captured of a window, before any scaling. It follows the size of the window's
    // inside once that has held.
    QSize pictureSize;
    Clock::time_point pictureDue;

    std::shared_ptr<GraphicsCaptureSession> session;
    // Not before this is a capture made again that ended by itself.
    Clock::time_point remakeAt;
    // Until then, or until one comes that has something in it, a picture of the window may
    // have nothing in it: the window was hidden a moment ago, or its capture has just begun.
    Clock::time_point emptyPicturesUntil;

    // For a window by duplication: the screen it is on, the card that screen is on, and the
    // pictures of the window on that card, which both ways of capturing it fill.
    HMONITOR screenHandle = nullptr;
    std::optional<Monitor> screen;
    bool screenChanged = true;
    AVBufferRef *device = nullptr;
    int deviceAdapter = -1;
    AVBufferRef *pool = nullptr;
    std::shared_ptr<WindowDrawer> drawer;
    quint64 cutOutPictures = 0;
    quint64 drawnPictures = 0;
    std::optional<bool> lastWasCutOut;
    // Why the last picture could not be cut out of the screen, for the log.
    const char *drawnBecause = "";

    PictureConverter converter;
    PictureConverter rescaler;
    CardScaler scaler;
    Media::FramePointer captured{ av_frame_alloc() };
    Media::FramePointer wholeScreen{ av_frame_alloc() };
    Media::FramePointer bgra{ av_frame_alloc() };
    Media::FramePointer asDrawn{ av_frame_alloc() };

    // How a screen's pictures last went on from the card, for the log.
    const char *wentOn = "";
    // `video/skip_unchanged_pictures`, for turning off what follows: a picture that duplication
    // or the window itself gives again unchanged is not delivered again.
    bool skipsUnchanged = true;
    // The screen's picture the last picture delivered was made of, and for a window where
    // in it the window was.
    Media::FramePointer lastOfScreen{ av_frame_alloc() };
    CutOut lastCut;
    // The size of the bitmap `bgra` was filled from, while the last picture delivered is that
    // one. Not valid otherwise.
    QSize drawingDelivered;
    QSize drawingInBgra;
};

bool screenCaptureAvailable()
{
    // Desktop duplication came with Windows 8.
    static const bool available = avfilter_get_by_name("ddagrab") && QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows8;
    return available;
}

bool windowCaptureAvailable()
{
    return graphicsCaptureUsable() || screenCaptureAvailable();
}

bool captureMethodUsable(CaptureMethod method)
{
    switch (method) {
    case CaptureMethod::GraphicsCapture:
        return graphicsCaptureUsable();
    case CaptureMethod::DesktopDuplication:
        return screenCaptureAvailable();
    case CaptureMethod::Portal:
    case CaptureMethod::X11:
        return false;
    case CaptureMethod::Auto:
        break;
    }
    return screenCaptureAvailable() || graphicsCaptureUsable();
}

QList<CaptureSource> captureSources(CaptureMethod)
{
    QList<CaptureSource> sources;
    if (!screenCaptureAvailable() && !graphicsCaptureUsable())
        return sources;

    const std::vector<Monitor> monitors = attachedMonitors();
    for (size_t i = 0; i < monitors.size(); i++) {
        CaptureSource source;
        source.id = screenSourceId(monitors[i]);
        source.name = captureText("Screen %1").arg(i + 1);
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

QSize encodeSizeOf(const CaptureSource &source, int maxHeight)
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
    tearDown();
    request = wanted;
    failingSince.reset();
    retryAt = {};
    closed = false;
    failed = false;
    cardScalingFailed = false;
    failureText.clear();

    backends = std::make_unique<Backends>();
    backends->window = windowOf(request.sourceId);
    backends->skipsUnchanged = QSettings().value(QStringLiteral("video/skip_unchanged_pictures"), true).toBool();
    if (usesGraphicsCapture(request.method, backends->window != nullptr))
        path = Path::GraphicsCapture;
    else
        path = backends->window ? Path::WindowByDuplication : Path::Duplication;
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
    // Only duplication of a screen has to be read to stay in time with it.
    if (!picturesWanted && path != Path::Duplication) {
        std::this_thread::sleep_for(pictureInterval(request.framerate));
        return Status::NoFrame;
    }
    switch (path) {
    case Path::GraphicsCapture:
        return readGraphicsCapture(into);
    case Path::WindowByDuplication:
        return readWindowByDuplication(into);
    case Path::Duplication:
        break;
    }
    return readDuplication(into);
}

// Whether the window is still there and showing, and how large the picture of it is to be.
// Status::Frame when there is a window to read.
ScreenCapture::Status ScreenCapture::watchWindow()
{
    Backends &state = *backends;
    if (!IsWindow(state.window)) {
        qCInfo(LogVideo) << "The window being captured was closed";
        tearDown();
        closed = true;
        return Status::Closed;
    }

    // Graphics capture leaves out by itself a window that has asked to be kept out of
    // captures. Duplication would show it black, and it is not asked to draw itself either.
    DWORD affinity = 0;
    const bool keptOutOfCaptures = path == Path::WindowByDuplication && GetWindowDisplayAffinity(state.window, &affinity) && affinity != 0;
    const QRect client = IsIconic(state.window) || !IsWindowVisible(state.window) || keptOutOfCaptures ? QRect() : clientOnScreen(state.window);
    if (client.isEmpty()) {
        // Hiding a window ends its graphics capture, and minimising it does not.
        if (state.session && state.session->endedWith() != 0)
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

void ScreenCapture::delivered()
{
    everDelivered = true;
    failingSince.reset();
}

// Whether pictures that have to be scaled, or converted for an encoder that takes none of the
// screen's own, stay on the card for it.
bool ScreenCapture::scalesOnCard() const
{
    return request.keepOnCard && !cardScalingFailed && CardScaler::usable();
}

// `area` of a picture on the card made `placed` of the one that is sent, which stays on the
// card. False when the card would not, after which this capture scales in software.
bool ScreenCapture::scaleOnCard(const AVFrame *from, const QRect &area, const QSize &size, const QRect &placed, AVFrame *into)
{
    const CardScaler::Output output = { size, request.bt601Only || !Media::usesBt709AtHeight(size.height()), request.rgbOnCard };
    if (backends->scaler.scale(from, area, output, placed, into))
        return true;
    cardScalingFailed = true;
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

    // Read whether or not a picture is wanted: ddagrab keeps the time between pictures, and
    // makes up for a read that did not come by handing the next ones over at once.
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
    // A picture that comes a little before one is due is the one a source at twice the rate
    // has ready, and the next would be late.
    const GraphicsCaptureSession::Taken taken = state.session->take(state.captured.get(), state.pictureDue - interval / 4, giveUpAt);
    if (taken == GraphicsCaptureSession::Taken::Ended) {
        const int ended = state.session->endedWith();
        if (ended != AVERROR_EOF || !state.window)
            return afterFailure(ended);
        // Windows ends the capture of a window that is hidden or closed. Neither is a failure,
        // and which it was the next look at the window tells.
        state.session.reset();
        state.remakeAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
        return Status::NoFrame;
    }
    if (taken == GraphicsCaptureSession::Taken::Nothing)
        return Status::NoFrame;
    state.pictureDue = std::max(state.pictureDue + interval, Clock::now() + interval / 2);
    // Graphics capture hands over a picture with nothing in it, transparent all over, as a
    // window is hidden or closed, and another as it is shown, before the window has drawn
    // itself. Sent on, it is black for as long as the window stays away. Those of before it
    // was hidden wait in the capture until it shows again, so how long after is no measure:
    // they are looked for until the first picture that has something in it.
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

// A picture on the card made what the encoder is to have: handed over as it is, scaled on the
// card, or read back and scaled in software. `captured` is left empty.
ScreenCapture::Status ScreenCapture::deliverFromCard(AVFrame *captured, AVFrame *into)
{
    Backends &state = *backends;
    const QSize ownSize(captured->width, captured->height);
    const QSize size = encodeSizeFor(ownSize, request.maxHeight);
    const char *wentOn = "read back and scaled in software";
    const bool bt601 = request.bt601Only || !Media::usesBt709AtHeight(size.height());
    if (request.keepOnCard && request.rgbOnCard && size == ownSize) {
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

// The official client's way with a window on Windows 10. Every picture is first looked for on
// the screen, and where the screen cannot give the window by itself the window draws that
// picture. Either way it ends up the same size and in the same place, so the encoder never
// knows which it was.
ScreenCapture::Status ScreenCapture::readWindowByDuplication(AVFrame *into)
{
    Backends &state = *backends;
    std::this_thread::sleep_until(state.pictureDue);
    state.pictureDue = std::max(state.pictureDue + pictureInterval(request.framerate), Clock::now());

    const QRect client = clientOnScreen(state.window);
    const bool screenDuplicated = duplicateScreenOf(client);
    const QSize size = encodeSizeFor(state.pictureSize, request.maxHeight);
    const bool asCaptured = state.pool && request.keepOnCard && request.rgbOnCard && size == state.pictureSize;
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
        // The window's own picture first: on the card if that is where it stays, in memory if not.
        if (cutOut)
            ready = cutOntoCard(whole, cut.onScreen, state.pool, cut.inPicture.topLeft(), card) && (staysOnCard || (prepareBgra(bgra, state.pictureSize) && av_hwframe_transfer_data(bgra, card, 0) >= 0));
        else if (staysOnCard)
            ready = av_hwframe_get_buffer(state.pool, card, 0) >= 0 && av_hwframe_transfer_data(card, bgra, 0) >= 0;

        const bool bt601 = !staysOnCard && (request.bt601Only || !Media::usesBt709AtHeight(size.height()));
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

// Sees to it that the screen under the middle of the window is being duplicated, and that
// there is a card to keep the window's pictures on. False when that screen cannot be read now.
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
        if (av_hwdevice_ctx_create(&state.device, AV_HWDEVICE_TYPE_D3D11VA, QByteArray::number(adapter).constData(), nullptr, 0) < 0)
            retryAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
    }
    if (state.device && !state.pool)
        state.pool = cardPictures(state.device, state.pictureSize);

    // ddagrab does not turn a turned screen's picture upright.
    if (!state.pool || !state.screen || state.screen->turned)
        return false;
    if (graph)
        return true;
    if (!retryDue)
        return false;

    const QByteArray chain = "ddagrab=output_idx=" + QByteArray::number(state.screen->output) + ":framerate=" + QByteArray::number(request.framerate);
    const int built = assemble(chain, state.device, &graph, &sink);
    if (built < 0) {
        // Until Windows gives the screen back, which it also takes for a mode it may come
        // back in another size with.
        state.screenChanged = true;
        retryAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
    }
    return built >= 0;
}

// Where the window's picture is in the screen's, which is then the picture read last. False
// when the screen does not show the window by itself, or not where it was when the screen was
// read.
bool ScreenCapture::cutOutOfScreen(const QRect &client, CutOut *cut)
{
    Backends &state = *backends;
    // While its size is changing, and where it is smaller than what is sent, the window does
    // not fill the picture.
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
            retryAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
        }
        return false;
    }

    // The read waits for the screen, and a window moved or covered meanwhile has left its
    // place there to something else.
    state.drawnBecause = "it moved or was covered while its screen was read";
    if (clientOnScreen(state.window) != client || isCoveredOnScreen(state.window, client, ShellSurfaces::LookedFor)) {
        av_frame_unref(whole);
        return false;
    }
    *cut = { inPicture.translated(client.topLeft() - screen.topLeft()), inPicture };
    return true;
}

// The window's picture as the window draws it, into the BGRA picture kept for it.
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

    // A program Windows scales for the screen draws itself at its own, smaller size.
    AVFrame *asDrawn = state.asDrawn.get();
    if (!prepareBgra(asDrawn, drawn.size()))
        return Drawing::Failed;
    copyTopLeft(drawn, asDrawn);
    av_frame_unref(bgra);
    return state.rescaler.convert(asDrawn, state.pictureSize, AV_PIX_FMT_BGRA, false, bgra) >= 0 ? Drawing::Drawn : Drawing::Failed;
}

// Neither source recovers from losing what it captures: ddagrab not from losing the screen nor
// from a changed screen mode, graphics capture not from its window being hidden. The capture
// has to be made again, and until Windows gives the source back that fails.
ScreenCapture::Status ScreenCapture::afterFailure(int error)
{
    tearDown();
    const QString reason = backends->window ? captureText("The window can no longer be captured (%1).") : captureText("The screen can no longer be captured (%1).");
    // Access is denied for as long as the secure desktop shows, which is as long as the
    // user leaves a UAC prompt open or the machine locked, and no reason to stop sharing.
    const bool secureDesktopShowing = error == AVERROR(EPERM) && everDelivered;
    return giveUpOrRetry(reason.arg(describeError(error)), secureDesktopShowing);
}

// The screen's pictures come out of the duplication on the card whatever is made of them
// after, because that is where one that shows nothing new can be told from the one before it.
int ScreenCapture::buildDuplication()
{
    const std::vector<Monitor> monitors = attachedMonitors();
    const Monitor *monitor = monitorOf(monitors, request.sourceId);
    if (!monitor)
        return AVERROR(ENODEV);

    // ddagrab counts outputs on the adapter of the device it is given.
    AVBufferRef *device = nullptr;
    int result = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, QByteArray::number(monitor->adapter).constData(), nullptr, 0);
    if (result < 0)
        return result;

    const QByteArray chain = "ddagrab=output_idx=" + QByteArray::number(monitor->output) + ":framerate=" + QByteArray::number(request.framerate);
    result = assemble(chain, device, &graph, &sink);
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

    // Graphics capture scales by itself, into BGRA pictures an encoder has to take as they are.
    // It stretches whatever it has captured over the whole picture, which is right for a source
    // that fills its picture, and a window smaller than the smallest picture does not.
    const QSize size = encodeSizeFor(ownSize, request.maxHeight);
    const bool asCaptured = request.keepOnCard && request.rgbOnCard && size == ownSize;
    const bool fillsItsPicture = !state.window || evenSize(state.clientSize) == ownSize;
    const bool scalesByItself = request.keepOnCard && request.rgbOnCard && size != ownSize && fillsItsPicture;
    const QSize captured = scalesByItself ? size : ownSize;

    // The size is given because the filter otherwise fixes it before the first picture, from
    // what Windows says the window measures, which is not always what it then hands over.
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
    // Left to choose, what graphics capture will not take is duplicated.
    const bool leftToChoose = request.method == CaptureMethod::Auto && state.window && !everDelivered;
    if ((result == AVERROR(ENOSYS) || leftToChoose) && screenCaptureAvailable()) {
        qCInfo(LogVideo) << "Graphics capture of" << request.sourceId << "failed (" << describeError(result) << "); using desktop duplication";
        path = state.window ? Path::WindowByDuplication : Path::Duplication;
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
