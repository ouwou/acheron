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

struct AVFilterContext;
struct AVFilterGraph;
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
// the official client's rule: a window can be cut out of the screen only when nothing overlaps it,
// its own menus and dialogs included
[[nodiscard]] bool windowIsCovered(const QList<StackedWindow> &topDown, int target);

constexpr int STILL_PICTURE_INTERVAL_MS = 500;
[[nodiscard]] bool stillPictureIsDue(qint64 msSinceLastPicture, bool keyFrameWaitedFor, int framerate);

// not thread safe
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
    // polled while a read waits on a window that may have stopped answering; false ends the wait
    void setStillWanted(std::function<bool()> wanted) { stillWanted = std::move(wanted); }
    [[nodiscard]] QString failure() const { return failureText; }

    [[nodiscard]] bool deliversOnlyChanges() const;
    [[nodiscard]] QString captureName() const;

    static constexpr int RETRY_INTERVAL_MS = 250;
    static constexpr int CARD_SCALING_RETRY_MS = 30'000;
    static constexpr int GIVE_UP_AFTER_MS = 5000;
    static constexpr int RESIZE_SETTLE_MS = 300;
    static constexpr int DRAWING_LIMIT_MS = 5000;

private:
    enum class Path {
        Duplication,
        GraphicsCapture,
        WindowByDuplication,
        ScreenCopy,
    };

    struct Backends;
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
    [[nodiscard]] bool scalesOnCard() const;
    bool scaleOnCard(const AVFrame *from, const QRect &area, const QSize &size, const QRect &placed, AVFrame *into);
    int buildDuplication();
    int buildGraphicsCapture();
    void dropDuplication();
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
    Path path = Path::Duplication;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
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
