#pragma once

#include <QPointer>
#include <QWidget>

#include <functional>

#include "UI/Call/CallTileModel.hpp"

class QKeyEvent;

namespace Acheron {
namespace Core {
class CallVideo;
class StreamSession;
} // namespace Core

namespace UI {

class VideoSurface;

// F11 and F toggle full screen in the call's windows, and Escape leaves it.
[[nodiscard]] bool togglesFullScreen(const QKeyEvent *event, bool fullScreen);
void paintBlack(QWidget *widget);

// One tile of a call view: the surface it draws on, what feeds that surface, and the window it
// is in while it is out of the view. Its stream or camera stops being drawn for it when it goes.
class CallTile
{
public:
    CallTile(const TileKey &key, QWidget *stage, Core::CallVideo *video);
    ~CallTile();

    CallTile(const CallTile &) = delete;
    CallTile &operator=(const CallTile &) = delete;

    [[nodiscard]] const TileKey &key() const { return tileKey; }
    [[nodiscard]] VideoSurface *surface() const { return drawnOn; }

    [[nodiscard]] Core::StreamSession *session() const { return watched; }
    // The stream's session, null while it is not being watched.
    void bind(Core::StreamSession *session);

    [[nodiscard]] bool hasCamera() const { return camera; }
    void setHasCamera(bool on);
    // Where the surface now is, for what draws on it to pick the size it decodes at.
    void announceGeometry(int pixelCount, bool visible);

    // A picture that is not a decoded frame, such as the preview of the account's own share.
    void present(const QImage &picture);

    [[nodiscard]] bool isPoppedOut() const { return !window.isNull(); }
    // `closed` is called when its window is closed, which should put it back.
    void popOut(const QString &title, std::function<void()> closed);
    void dockIn(QWidget *stage);
    void toggleFullScreen();

private:
    TileKey tileKey;
    VideoSurface *drawnOn;
    QPointer<Core::CallVideo> video;
    QPointer<Core::StreamSession> watched;
    bool camera = false;
    QPointer<QWidget> window;
};

} // namespace UI
} // namespace Acheron
