#pragma once

#include <QImage>
#include <QList>
#include <QSize>
#include <QString>

#include <chrono>
#include <functional>
#include <memory>

#include "Core/Video/CaptureSources.hpp"

class QObject;
struct AVFrame;

namespace Acheron {
namespace Core {
namespace Video {

// One screen or window being captured. Its pictures are unscaled RGB: BGRA or BGR0 in system
// memory, or AV_PIX_FMT_DRM_PRIME ones still on the graphics card. Not thread safe.
class LinuxGrabber
{
public:
    enum class Result {
        Frame,
        NoFrame,
        // The window is minimised or hidden and shows nothing until it is brought back.
        Hidden,
        // The window was closed, or the desktop ended the capture.
        Closed,
        Failed,
    };

    virtual ~LinuxGrabber() = default;

    // The newest picture not handed over yet, waited for no longer than `wait`. It stays as it
    // is for as long as the caller keeps it.
    virtual Result grab(AVFrame *into, std::chrono::milliseconds wait) = 0;
    // True for a capture that has a picture only when the source shows something new.
    [[nodiscard]] virtual bool deliversOnlyChanges() const = 0;
    [[nodiscard]] virtual QString name() const = 0;
    [[nodiscard]] virtual QString failure() const = 0;
};

// An X11 display's screens and windows, listed through xcb and captured by ffmpeg's x11grab.
// Source ids are "screen:<RandR monitor name>" and "window:<id>".
namespace X11Capture {

// False without an X server that shows the desktop: none at all, or Xwayland.
[[nodiscard]] bool usable();
[[nodiscard]] QList<CaptureSource> screens();
[[nodiscard]] QList<CaptureSource> windows();
[[nodiscard]] QImage thumbnail(const CaptureSource &source, const QSize &bound);
// The size of the pictures `source` gives now. Empty for a window that is hidden or gone.
[[nodiscard]] QSize sizeOf(const CaptureSource &source);
void restoreIfMinimised(const CaptureSource &window);
[[nodiscard]] std::unique_ptr<LinuxGrabber> open(const QString &sourceId, int framerate);

} // namespace X11Capture

// The desktop portal's screen cast, handed over as a PipeWire stream. Its own dialog does the
// choosing: a source is asked for by kind and comes back with the id "pipewire:<n>".
namespace PortalCapture {

[[nodiscard]] bool usable();
// `chosen` is called once, in `receiver`'s thread and only while it lives: with the source, or
// with an empty id and what went wrong, which is nothing when the user called it off.
void choose(CaptureSource::Kind kind, QObject *receiver, std::function<void(const CaptureSource &, const QString &)> chosen);
// The same without the dialog where `token`, from an earlier choice, still stands. Before
// `chosen` is called with a source, the token for the next time is put in its place.
void chooseAgain(CaptureSource::Kind kind, QString *token, QObject *receiver, std::function<void(const CaptureSource &, const QString &)> chosen);
// A stream of this process's own PipeWire connection as a source, for trying the capture out.
[[nodiscard]] CaptureSource adopt(quint32 node, const QSize &size);
[[nodiscard]] QSize sizeOf(const CaptureSource &source);
// With `onCard`, pictures the desktop keeps on the graphics card are handed over there.
[[nodiscard]] std::unique_ptr<LinuxGrabber> open(const QString &sourceId, int framerate, bool onCard);
// Ends the desktop's sharing of a chosen source. Any thread.
void release(const QString &sourceId);

} // namespace PortalCapture

} // namespace Video
} // namespace Core
} // namespace Acheron
