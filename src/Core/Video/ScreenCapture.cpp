#include "Core/Video/ScreenCapture.hpp"

#include "Core/Logging.hpp"
#include "Core/Video/CaptureCommon.hpp"

#include <QSettings>

#include <algorithm>

// Where the platforms' own capture is, in ScreenCaptureWindows.cpp and ScreenCaptureLinux.cpp.
#if defined(Q_OS_WIN) && defined(ACHERON_HAVE_AVFILTER)
#define ACHERON_WINDOWS_SCREEN_CAPTURE
#endif
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && defined(ACHERON_HAVE_AVFILTER)
#define ACHERON_LINUX_SCREEN_CAPTURE
#endif

namespace Acheron {
namespace Core {
namespace Video {

namespace {

const char *const CAPTURE_METHOD_KEY = "video/capture_method";
const QLatin1String AUTO_METHOD("auto");
const QLatin1String GRAPHICS_CAPTURE_METHOD("graphics_capture");
const QLatin1String DESKTOP_DUPLICATION_METHOD("desktop_duplication");
const QLatin1String PORTAL_METHOD("portal");
const QLatin1String X11_METHOD("x11");

} // namespace

QSize encodeSizeFor(const QSize &source, int maxHeight)
{
    QSize size = source;
    const QSize bound(maxHeight * 16 / 9, maxHeight);
    if (maxHeight > 0 && (source.width() > bound.width() || source.height() > bound.height()))
        size = source.scaled(bound, Qt::KeepAspectRatio);
    return QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
}

CaptureMethod captureMethod()
{
    const QString stored = QSettings().value(CAPTURE_METHOD_KEY).toString();
    if (stored == GRAPHICS_CAPTURE_METHOD)
        return CaptureMethod::GraphicsCapture;
    if (stored == DESKTOP_DUPLICATION_METHOD)
        return CaptureMethod::DesktopDuplication;
    if (stored == PORTAL_METHOD)
        return CaptureMethod::Portal;
    if (stored == X11_METHOD)
        return CaptureMethod::X11;
    return CaptureMethod::Auto;
}

void setCaptureMethod(CaptureMethod method)
{
    QLatin1String stored = AUTO_METHOD;
    if (method == CaptureMethod::GraphicsCapture)
        stored = GRAPHICS_CAPTURE_METHOD;
    else if (method == CaptureMethod::DesktopDuplication)
        stored = DESKTOP_DUPLICATION_METHOD;
    else if (method == CaptureMethod::Portal)
        stored = PORTAL_METHOD;
    else if (method == CaptureMethod::X11)
        stored = X11_METHOD;
    QSettings().setValue(CAPTURE_METHOD_KEY, QString(stored));
}

#if !defined(ACHERON_LINUX_SCREEN_CAPTURE)
bool isChosenByTheDesktop(const CaptureSource &)
{
    return false;
}

bool isHandedOverByTheDesktop(const CaptureSource &)
{
    return false;
}

void chooseSource(const CaptureSource &source, QObject *, std::function<void(const CaptureSource &, const QString &)> chosen)
{
    chosen(source, QString());
}

void releaseSource(const CaptureSource &)
{
}
#endif

bool stillPictureIsDue(qint64 msSinceLastPicture, bool keyFrameWaitedFor, int framerate)
{
    return msSinceLastPicture >= STILL_PICTURE_INTERVAL_MS || (keyFrameWaitedFor && msSinceLastPicture >= 1000 / std::max(1, framerate));
}

bool windowIsCovered(const QList<StackedWindow> &topDown, int target)
{
    if (target < 0 || target >= topDown.size())
        return true;
    const StackedWindow &window = topDown[target];
    if (window.bounds.isEmpty() || window.minimised || window.cloaked)
        return true;

    for (int i = 0; i < target; i++) {
        const StackedWindow &above = topDown[i];
        if (!above.visible || above.minimised)
            continue;
        if (above.livePreview)
            return true;
        if (above.overlay || above.cloaked || above.tabProxy)
            continue;
        if (above.bounds.intersects(window.bounds))
            return true;
    }
    return false;
}

ScreenCapture::Status ScreenCapture::giveUpOrRetry(const QString &reason, bool clockStartsOver)
{
    const auto now = CaptureClock::now();
    if (!failingSince || clockStartsOver)
        failingSince = now;
    if (now - *failingSince >= std::chrono::milliseconds(GIVE_UP_AFTER_MS)) {
        failed = true;
        failureText = reason;
        qCWarning(LogVideo) << "Giving up on capturing" << request.sourceId << ":" << reason;
        return Status::Failed;
    }
    retryAt = now + std::chrono::milliseconds(RETRY_INTERVAL_MS);
    return Status::Stalled;
}

#if !defined(ACHERON_WINDOWS_SCREEN_CAPTURE) && !defined(ACHERON_LINUX_SCREEN_CAPTURE)

struct ScreenCapture::Backends
{
};

bool screenCaptureAvailable()
{
    return false;
}

bool windowCaptureAvailable()
{
    return false;
}

bool captureMethodUsable(CaptureMethod)
{
    return false;
}

QList<CaptureSource> captureSources(CaptureMethod)
{
    return {};
}

QList<CaptureSource> captureWindows(CaptureMethod)
{
    return {};
}

QImage captureThumbnail(const CaptureSource &, const QSize &)
{
    return {};
}

void restoreIfMinimised(const CaptureSource &)
{
}

QSize encodeSizeOf(const CaptureSource &, int)
{
    return {};
}

ScreenCapture::ScreenCapture() = default;

ScreenCapture::~ScreenCapture() = default;

void ScreenCapture::configure(const Request &wanted)
{
    request = wanted;
}

bool ScreenCapture::deliversOnlyChanges() const
{
    return false;
}

QString ScreenCapture::captureName() const
{
    return {};
}

ScreenCapture::Status ScreenCapture::read(AVFrame *)
{
    failureText = captureText("This build cannot capture the screen.");
    return Status::Failed;
}

#endif

} // namespace Video
} // namespace Core
} // namespace Acheron
