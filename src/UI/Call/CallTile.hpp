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

[[nodiscard]] bool togglesFullScreen(const QKeyEvent *event, bool fullScreen);
void paintBlack(QWidget *widget);

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
    void bind(Core::StreamSession *session);

    [[nodiscard]] bool hasCamera() const { return camera; }
    void setHasCamera(bool on);
    void announceGeometry(int pixelCount, bool visible);

    void present(const QImage &picture);

    [[nodiscard]] bool isPoppedOut() const { return !window.isNull(); }
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
