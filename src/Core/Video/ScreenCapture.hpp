#pragma once

#include <QList>
#include <QRect>
#include <QSize>
#include <QString>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include "Core/Video/CaptureSources.hpp"
#include "Core/Video/ColourMatrix.hpp"

#if defined(Q_OS_WIN) && defined(ACHERON_HAVE_AVFILTER)
#define ACHERON_WINDOWS_SCREEN_CAPTURE
#elif defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && defined(ACHERON_HAVE_AVFILTER)
#define ACHERON_LINUX_SCREEN_CAPTURE
#endif

struct AVFrame;

namespace Acheron {
namespace Core {
namespace Video {

[[nodiscard]] QSize encodeSizeFor(const QSize &source, std::optional<int> maxHeight);

struct StackedWindow
{
    QRect bounds;
    bool visible = true;
    bool minimised = false;
    bool cloaked = false;
    bool overlay = false;
    bool tabProxy = false;
    bool livePreview = false;
};
// the official client's rule, which counts the window's own menus and dialogs as covering it
[[nodiscard]] bool windowIsCovered(const QList<StackedWindow> &topDown, int target);

class ScreenCapture
{
public:
    struct Request
    {
        QString sourceId;
        CaptureMethod method = CaptureMethod::Auto;
        std::optional<int> maxHeight;
        int framerate = 30;
        int softwareFormat = -1;
        bool keepOnCard = false;
        bool rgbOnCard = false;
        bool bt601Only = false;
    };

    enum class Status {
        Frame,
        NoFrame,
        Stalled,
        Hidden,
        Closed,
        Failed,
    };

    ScreenCapture();
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture &) = delete;
    ScreenCapture &operator=(const ScreenCapture &) = delete;

    void configure(const Request &request);
    Status read(AVFrame *into);
    void setPicturesWanted(bool wanted) { picturesWanted = wanted; }
    void setStillWanted(std::function<bool()> wanted) { stillWanted = std::move(wanted); }
    [[nodiscard]] QString failure() const { return failureText; }

    [[nodiscard]] bool deliversOnlyChanges() const;
    [[nodiscard]] QString captureName() const;

    static constexpr int RETRY_INTERVAL_MS = 250;
    static constexpr int GIVE_UP_AFTER_MS = 5000;
    static constexpr int RESIZE_SETTLE_MS = 300;

private:
    struct Backends;

#ifdef ACHERON_WINDOWS_SCREEN_CAPTURE
    struct CutOut;
    enum class Drawing;

    Status watchWindow();
    Status readDuplication(AVFrame *into);
    Status readGraphicsCapture(AVFrame *into);
    Status readWindowByDuplication(AVFrame *into);
    Status readScreenCopy(AVFrame *into);
    bool cutOutOfScreen(const QRect &client, CutOut *cut);
    bool duplicateScreenOf(const QRect &client);
    Drawing drawnByItself(const QRect &client);
    Status deliverFromCard(AVFrame *captured, AVFrame *into);
    bool scaleOnCard(const AVFrame *from, const QRect &area, const QSize &size, const QRect &placed, AVFrame *into);
    int buildDuplication();
    int buildGraphicsCapture();
    void dropDuplication();
#endif
    [[nodiscard]] bool scalesOnCard() const;
    [[nodiscard]] ColourMatrix matrixFor(const QSize &size) const;
    void startOver(const Request &wanted);
    void tearDown();
    void delivered();
    Status afterFailure(int error);
    Status giveUpOrRetry(const QString &reason, bool clockStartsOver);
    void retryLater();
    void suspendCardScaling();
    [[nodiscard]] bool cardScalingSuspended() const;

    Request request;
    std::unique_ptr<Backends> backends;
    bool picturesWanted = true;
    std::function<bool()> stillWanted;
    bool everDelivered = false;
    std::optional<std::chrono::steady_clock::time_point> cardScalingResumesAt;
    std::optional<std::chrono::steady_clock::time_point> failingSince;
    std::chrono::steady_clock::time_point retryAt;
    bool closed = false;
    bool failed = false;
    QString failureText;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
