#pragma once

#include <QImage>
#include <QList>
#include <QRect>
#include <QSize>
#include <QString>

#include <functional>

class QObject;

namespace Acheron {
namespace Core {
namespace Video {

// Something that can be shared: a screen, or one window.
struct CaptureSource
{
    enum class Kind {
        Screen,
        Window,
    };

    // "screen:<n>", where n is stable for as long as the monitors stay as they are, or
    // "window:<handle>".
    QString id;
    Kind kind = Kind::Screen;
    // A screen's number, or a window's title.
    QString name;
    // In the desktop's own pixels.
    QRect geometry;
    // The process a window belongs to, which is whose sound goes with it. Zero for a screen.
    quint32 processId = 0;
};

// How pictures are taken off the machine. Remembered as `video/capture_method`.
enum class CaptureMethod {
    // A screen by desktop duplication, a window by graphics capture where Windows has it.
    Auto,
    // Windows.Graphics.Capture: a window is captured by itself whatever covers it, and
    // Windows 10 draws a yellow border around what is being captured.
    GraphicsCapture,
    // No border. A window is cut out of its screen while no window above overlaps it, and is
    // asked to draw itself otherwise, as the official client does on Windows 10.
    DesktopDuplication,
    // Linux: what the user chooses in the desktop portal's own dialog, handed over by PipeWire.
    Portal,
    // Linux: an X11 display's screens and windows, grabbed from the X server.
    X11,
};
[[nodiscard]] CaptureMethod captureMethod();
void setCaptureMethod(CaptureMethod method);
// False for a method this build or this Windows cannot use. A share asked to use one is
// captured by a method that can be used.
[[nodiscard]] bool captureMethodUsable(CaptureMethod method);

// False where this build or this machine cannot capture a screen at all, in which case nothing
// offers to share one.
[[nodiscard]] bool screenCaptureAvailable();
// The same for single windows. A build or a Windows too old to capture one has none to offer.
[[nodiscard]] bool windowCaptureAvailable();

// What a method has to offer, by default the one the settings name. The portal lists nothing:
// it offers one screen and one window, each standing for whatever the user goes on to choose
// in the desktop's dialog, and beside each, once the desktop remembers a choice, one that
// stands for the same again.
[[nodiscard]] QList<CaptureSource> captureSources(CaptureMethod method = captureMethod());
[[nodiscard]] QList<CaptureSource> captureWindows(CaptureMethod method = captureMethod());

// A still of the source as it is now, no larger than `bound`, for a picker. Null when it
// cannot be had.
[[nodiscard]] QImage captureThumbnail(const CaptureSource &source, const QSize &bound);

// The size `source` would be sent at as it is now: a screen whole, a window by its inside.
// Empty for a window that has nothing to measure: one that is hidden, minimised or gone.
[[nodiscard]] QSize encodeSizeOf(const CaptureSource &source, int maxHeight);

[[nodiscard]] bool isChosenByTheDesktop(const CaptureSource &source);
// Whether `source` is what the user chose there. The desktop does not say which program a
// window it hands over belongs to.
[[nodiscard]] bool isHandedOverByTheDesktop(const CaptureSource &source);
// Has the user choose in the desktop's dialog where `source` stands for that. `chosen` gets the
// source to share, or an empty id and what went wrong: nothing when the user called it off.
void chooseSource(const CaptureSource &source, QObject *receiver, std::function<void(const CaptureSource &, const QString &)> chosen);
// Ends the desktop's sharing of a source it handed over. Any other source is left alone.
void releaseSource(const CaptureSource &source);

// A minimised window shows nothing to capture until it is brought back.
void restoreIfMinimised(const CaptureSource &window);

} // namespace Video
} // namespace Core
} // namespace Acheron
