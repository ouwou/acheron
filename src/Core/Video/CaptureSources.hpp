#pragma once

#include <QImage>
#include <QList>
#include <QRect>
#include <QSize>
#include <QString>

#include <functional>
#include <optional>

class QObject;

namespace Acheron {
namespace Core {
namespace Video {

inline const QLatin1String SCREEN_SOURCE_PREFIX("screen:");
inline const QLatin1String WINDOW_SOURCE_PREFIX("window:");
inline const QLatin1String PIPEWIRE_SOURCE_PREFIX("pipewire:");

[[nodiscard]] inline QString windowSourceId(quint64 handle)
{
    return WINDOW_SOURCE_PREFIX + QString::number(handle);
}

[[nodiscard]] inline std::optional<quint64> windowHandleOf(const QString &sourceId)
{
    if (!sourceId.startsWith(WINDOW_SOURCE_PREFIX))
        return std::nullopt;
    bool numbered = false;
    const quint64 handle = sourceId.mid(WINDOW_SOURCE_PREFIX.size()).toULongLong(&numbered);
    return numbered ? std::optional<quint64>(handle) : std::nullopt;
}

struct CaptureSource
{
    enum class Kind {
        Screen,
        Window,
    };

    // "screen:<output name>", stable while other screens come and go, or "window:<handle>"
    QString id;
    Kind kind = Kind::Screen;
    QString name;
    QRect geometry;
    quint32 processId = 0;
};

enum class CaptureMethod {
    Auto,
    // Windows 10 draws a yellow border around the captured window
    GraphicsCapture,
    // a window is cut out of its screen while nothing overlaps it and drawn by itself otherwise, as the
    // official client does on Windows 10
    DesktopDuplication,
    // the only way Windows 7 has; what draws straight to the screen, like a full-screen game, comes out black
    ScreenCopy,
    Portal,
    X11,
};
[[nodiscard]] CaptureMethod captureMethod();
void setCaptureMethod(CaptureMethod method);
[[nodiscard]] bool captureMethodUsable(CaptureMethod method);

[[nodiscard]] bool screenCaptureAvailable();
[[nodiscard]] bool windowCaptureAvailable();

[[nodiscard]] QList<CaptureSource> captureSources(CaptureMethod method = captureMethod());
[[nodiscard]] QList<CaptureSource> captureWindows(CaptureMethod method = captureMethod());

[[nodiscard]] QImage captureThumbnail(const CaptureSource &source, const QSize &bound);

[[nodiscard]] QSize encodeSizeOf(const CaptureSource &source, std::optional<int> maxHeight);

[[nodiscard]] bool isChosenByTheDesktop(const CaptureSource &source);
// the desktop doesnt say which program a window it hands over belongs to
[[nodiscard]] bool isHandedOverByTheDesktop(const CaptureSource &source);
void chooseSource(const CaptureSource &source, QObject *receiver, std::function<void(const CaptureSource &, const QString &)> chosen);
void releaseSource(const CaptureSource &source);

void restoreIfMinimised(const CaptureSource &window);

} // namespace Video
} // namespace Core
} // namespace Acheron
