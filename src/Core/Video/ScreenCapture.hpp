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

// The size a source is sent at: fitted inside (maxHeight * 16 / 9) x maxHeight with its shape
// kept, never enlarged, and both sides even. A `maxHeight` of zero is the source's own size.
[[nodiscard]] QSize encodeSizeFor(const QSize &source, int maxHeight);

// A top-level window as far as it matters to whether it hides part of another.
struct StackedWindow
{
    // In the desktop's own pixels: the inside of the window in question, and all that shows of
    // any other.
    QRect bounds;
    bool visible = true;
    bool minimised = false;
    // Kept from the screen by Windows, as a window on another virtual desktop is.
    bool cloaked = false;
    // WS_EX_TRANSPARENT or WS_EX_NOACTIVATE: an overlay, or something that never comes forward.
    bool overlay = false;
    // Windows' stand-in for a browser tab among the taskbar's previews.
    bool tabProxy = false;
    // The taskbar's preview of a window at its full size, which shows over everything.
    bool livePreview = false;
};
// Whether the window at `target`, in a stack listed from the top down, cannot be cut out of
// the screen by itself: it shows nothing there, or a window above it overlaps it at all. The
// official client's rule, by which a window's own menus and dialogs cover it as well.
[[nodiscard]] bool windowIsCovered(const QList<StackedWindow> &topDown, int target);

// How often the last picture of a source that has gone still is sent again, so that whoever
// starts watching has something to look at.
constexpr int STILL_PICTURE_INTERVAL_MS = 500;
// Whether the last picture of a source that shows nothing new is to be encoded again, so long
// after it last was. A key frame someone is waiting for does not wait for the source to stir,
// but does keep to the frame rate.
[[nodiscard]] bool stillPictureIsDue(qint64 msSinceLastPicture, bool keyFrameWaitedFor, int framerate);

// Captures one screen or one window at a steady rate at most, scaled and converted for an
// encoder. It scales on the graphics card when that works and in software when it does not,
// and builds its capture anew whenever Windows takes the source away, which it does to a screen
// on a mode change, on a switch to the secure desktop, and when a full-screen program takes the
// output over. A window is followed as it moves and as it changes size, and what is captured
// of it is the inside of its frame.
//
// Not thread safe; it belongs to the thread that reads from it.
class ScreenCapture
{
public:
    struct Request
    {
        QString sourceId;
        CaptureMethod method = CaptureMethod::Auto;
        int maxHeight = 0;
        int framerate = 30;
        // The format (an AVPixelFormat, NV12 or YUV420P) of pictures in system memory.
        int softwareFormat = -1;
        // Pictures are left on the graphics card as Direct3D 11 NV12 frames whenever they can
        // be scaled there, and arrive in `softwareFormat` otherwise.
        bool keepOnCard = false;
        // With `keepOnCard`: a source sent at its own size, or one that graphics capture scales
        // by itself, is handed over as the BGRA frames it is captured in, marked with the
        // colours the encoder is to convert them to.
        bool rgbOnCard = false;
        // For a codec that cannot say which colours it carries and is taken as BT.601.
        bool bt601Only = false;
    };

    enum class Status {
        Frame,
        // Nothing new yet; ask again.
        NoFrame,
        // The source cannot be captured at the moment; ask again. Each call waits a little.
        Stalled,
        // The window is minimised or hidden and shows nothing until it is brought back; ask
        // again. Each call waits a little.
        Hidden,
        // The window was closed.
        Closed,
        Failed,
    };

    ScreenCapture();
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture &) = delete;
    ScreenCapture &operator=(const ScreenCapture &) = delete;

    void configure(const Request &request);
    // Blocks until the next picture is due.
    Status read(AVFrame *into);
    // While no picture is wanted the source is still read, as often as before, for whether it
    // is still there. What a picture costs to scale and convert is left out, and each read
    // says there is nothing new.
    void setPicturesWanted(bool wanted) { picturesWanted = wanted; }
    // Asked, any number of times a second, while a read waits for a window that may have
    // stopped answering: once it says no, the read stops waiting and says nothing came.
    void setStillWanted(std::function<bool()> wanted) { stillWanted = std::move(wanted); }
    [[nodiscard]] QString failure() const { return failureText; }

    // Whether a picture comes only when the source shows something new. The last one may
    // then be kept, and is what a viewer is to be sent again while nothing comes.
    [[nodiscard]] bool deliversOnlyChanges() const;
    // What the pictures have come from, for the statistics. For a window captured by
    // duplication it counts those cut out of the screen and those the window drew itself.
    [[nodiscard]] QString captureName() const;

    static constexpr int RETRY_INTERVAL_MS = 250;
    static constexpr int CARD_SCALING_RETRY_MS = 30'000;
    static constexpr int GIVE_UP_AFTER_MS = 5000;
    // How long a window's size has to hold before the picture is made anew for it, so that
    // dragging an edge does not start the encoder over at every step.
    static constexpr int RESIZE_SETTLE_MS = 300;
    // How long a window is given to draw itself. One that takes longer has stopped answering.
    static constexpr int DRAWING_LIMIT_MS = 5000;

private:
    enum class Path {
        Duplication,
        GraphicsCapture,
        // A window by duplication: cut out of its screen, or drawn by itself, picture by picture.
        WindowByDuplication,
    };

    struct Backends;
    struct CutOut;
    enum class Drawing;

    Status watchWindow();
    Status readDuplication(AVFrame *into);
    Status readGraphicsCapture(AVFrame *into);
    Status readWindowByDuplication(AVFrame *into);
    bool cutOutOfScreen(const QRect &client, CutOut *cut);
    bool duplicateScreenOf(const QRect &client);
    Drawing drawnByItself(const QRect &client);
    Status deliverFromCard(AVFrame *captured, AVFrame *into);
    [[nodiscard]] bool scalesOnCard() const;
    bool scaleOnCard(const AVFrame *from, const QRect &area, const QSize &size, const QRect &placed, AVFrame *into);
    int buildDuplication();
    int buildGraphicsCapture();
    void dropDuplication();
    void tearDown();
    void delivered();
    Status afterFailure(int error);
    // Tries again in a while, or gives up when the source has failed for long enough, with
    // `reason`. `clockStartsOver` counts the failure as the first of a new run.
    Status giveUpOrRetry(const QString &reason, bool clockStartsOver);
    // The card stopped scaling in the middle of this capture, which goes on in software for a
    // while: a card that only turned down one picture, or was taken away for a moment, is
    // tried again after.
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
