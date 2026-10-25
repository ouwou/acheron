#include "Core/Video/ScreenCapture.hpp"

#include "Core/Logging.hpp"
#include "Core/Video/CaptureCommon.hpp"
#include "Core/Video/CardScaler.hpp"

#include <QSettings>

#include <algorithm>
#include <array>

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

struct StoredMethod
{
    CaptureMethod method;
    QLatin1String name;
};

const std::array<StoredMethod, 6> STORED_METHODS = { {
        { CaptureMethod::Auto, QLatin1String("auto") },
        { CaptureMethod::GraphicsCapture, QLatin1String("graphics_capture") },
        { CaptureMethod::DesktopDuplication, QLatin1String("desktop_duplication") },
        { CaptureMethod::ScreenCopy, QLatin1String("screen_copy") },
        { CaptureMethod::Portal, QLatin1String("portal") },
        { CaptureMethod::X11, QLatin1String("x11") },
} };

} // namespace

QSize encodeSizeFor(const QSize &source, std::optional<int> maxHeight)
{
    return maxHeight ? fittedInside(source, QSize(*maxHeight * 16 / 9, *maxHeight)) : evenSize(source);
}

CaptureMethod captureMethod()
{
    const QString stored = QSettings().value(CAPTURE_METHOD_KEY).toString();
    const auto found = std::find_if(STORED_METHODS.begin(), STORED_METHODS.end(), [&stored](const StoredMethod &each) { return stored == each.name; });
    return found == STORED_METHODS.end() ? CaptureMethod::Auto : found->method;
}

void setCaptureMethod(CaptureMethod method)
{
    const auto found = std::find_if(STORED_METHODS.begin(), STORED_METHODS.end(), [method](const StoredMethod &each) { return method == each.method; });
    QSettings().setValue(CAPTURE_METHOD_KEY, QString(found == STORED_METHODS.end() ? STORED_METHODS.front().name : found->name));
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
    const auto now = std::chrono::steady_clock::now();
    if (!failingSince || clockStartsOver)
        failingSince = now;
    if (now - *failingSince >= std::chrono::milliseconds(GIVE_UP_AFTER_MS)) {
        failed = true;
        failureText = reason;
        qCWarning(LogVideo) << "Giving up on capturing" << request.sourceId << ":" << reason;
        return Status::Failed;
    }
    retryLater();
    return Status::Stalled;
}

void ScreenCapture::retryLater()
{
    retryAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
}

void ScreenCapture::suspendCardScaling()
{
    cardScalingResumesAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(CARD_SCALING_RETRY_MS);
}

bool ScreenCapture::cardScalingSuspended() const
{
    return cardScalingResumesAt && std::chrono::steady_clock::now() < *cardScalingResumesAt;
}

#if defined(ACHERON_WINDOWS_SCREEN_CAPTURE) || defined(ACHERON_LINUX_SCREEN_CAPTURE)

void ScreenCapture::startOver(const Request &wanted)
{
    tearDown();
    request = wanted;
    failingSince.reset();
    retryAt = {};
    closed = false;
    failed = false;
    cardScalingResumesAt.reset();
    failureText.clear();
}

void ScreenCapture::delivered()
{
    everDelivered = true;
    failingSince.reset();
}

bool ScreenCapture::scalesOnCard() const
{
    return request.keepOnCard && !cardScalingSuspended() && CardScaler::usable();
}

#endif

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

QSize encodeSizeOf(const CaptureSource &, std::optional<int>)
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
