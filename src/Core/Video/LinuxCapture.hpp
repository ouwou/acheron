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

// not thread safe
class LinuxGrabber
{
public:
    enum class Result {
        Frame,
        NoFrame,
        Hidden,
        Closed,
        Failed,
    };

    virtual ~LinuxGrabber() = default;

    virtual Result grab(AVFrame *into, std::chrono::milliseconds wait) = 0;
    [[nodiscard]] virtual bool deliversOnlyChanges() const = 0;
    [[nodiscard]] virtual QString name() const = 0;
    [[nodiscard]] virtual QString failure() const = 0;
};

namespace X11Capture {

// xwayland doesnt show the desktop
[[nodiscard]] bool usable();
[[nodiscard]] QList<CaptureSource> screens();
[[nodiscard]] QList<CaptureSource> windows();
[[nodiscard]] QImage thumbnail(const CaptureSource &source, const QSize &bound);
[[nodiscard]] QSize sizeOf(const CaptureSource &source);
void restoreIfMinimised(const CaptureSource &window);
[[nodiscard]] std::unique_ptr<LinuxGrabber> open(const QString &sourceId, int framerate);

} // namespace X11Capture

namespace PortalCapture {

[[nodiscard]] bool usable();
// called once, on `receiver`'s thread and only while it lives
void choose(CaptureSource::Kind kind, QObject *receiver, std::function<void(const CaptureSource &, const QString &)> chosen);
// the next token replaces `token` before `chosen` is called with a source
void chooseAgain(CaptureSource::Kind kind, QString *token, QObject *receiver, std::function<void(const CaptureSource &, const QString &)> chosen);
[[nodiscard]] CaptureSource adopt(quint32 node, const QSize &size);
[[nodiscard]] QSize sizeOf(const CaptureSource &source);
[[nodiscard]] std::unique_ptr<LinuxGrabber> open(const QString &sourceId, int framerate, bool onCard);
// any thread
void release(const QString &sourceId);

} // namespace PortalCapture

} // namespace Video
} // namespace Core
} // namespace Acheron
