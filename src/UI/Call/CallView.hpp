#pragma once

#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "Core/Snowflake.hpp"
#include "Core/Streams/StreamSession.hpp"
#include "UI/AvatarRequestTracker.hpp"
#include "UI/Call/CallTileModel.hpp"

namespace Acheron {
namespace Core {
class CallVideo;
class CameraSender;
class StreamBroadcaster;
class StreamWatcher;
namespace Audio {
class VoiceManager;
}
} // namespace Core

namespace UI {

class CallControlsBar;
class CallTile;

// The call an account is in: a tile for each stream, each camera and, optionally, everyone
// else. It sits in the main window, in `dock`, for as long as the call lasts, and can be
// collapsed to its bar there. A call without video starts as the bar alone and opens when
// video starts, unless the viewer collapsed it by hand during the call. The whole of it, or
// any one tile, can be moved out into a window of its own and back.
//
// Clicking a stream that is not being watched watches it, and a stream that starts being
// watched is enlarged unless a camera is. Double-clicking a tile enlarges it over a strip of the
// others.
//
// The account's own screen share has a tile like any other stream, drawn from the capture's
// previews. A share that ended by itself leaves its tile up to say why, until it is clicked
// away. The account's own camera shows mirrored, as its owner expects to see themself; what is
// sent is not.
class CallView : public QWidget
{
    Q_OBJECT
public:
    using NameResolver = std::function<QString(Core::Snowflake)>;
    using AvatarResolver = std::function<QUrl(Core::Snowflake)>;

    // `dock` is the widget whose layout the view lives in while it is not in its own window.
    CallView(Core::Audio::VoiceManager *voice, Core::StreamWatcher *watcher, Core::StreamBroadcaster *broadcaster, Core::CameraSender *camera, Core::ImageManager *images, Core::Snowflake accountId, NameResolver names, AvatarResolver avatars, const QString &title, QWidget *dock);
    ~CallView() override;

    // The voice channel the view was made for.
    [[nodiscard]] Core::Snowflake channelId() const { return channel; }

    // Whether anyone in the call has a camera on or a stream up.
    [[nodiscard]] bool hasVideo() const { return videoPresent; }

    [[nodiscard]] bool isPoppedOut() const { return poppedOut; }
    void setPoppedOut(bool out);

    [[nodiscard]] bool isCollapsed() const { return collapsed; }
    void setCollapsed(bool value);
    // Brings the view into sight: opened up where it is docked, or its window raised.
    void reveal();

    static constexpr QSize AVATAR_SIZE = QSize(128, 128);

signals:
    // Whether the view takes up room in its dock has changed.
    void presenceChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void rebuild();
    void addTile(const TileKey &key);
    void removeTile(const TileKey &key);
    [[nodiscard]] CallTile *tileAt(const TileKey &key) const;
    void refreshTile(CallTile &tile);
    void bindSession(CallTile &tile, Core::StreamSession *session);
    void applySessionState(CallTile &tile);
    void applyOwnShareState(CallTile &tile);
    void applyOwnCameraState(CallTile &tile);
    void showOwnPreview(const QImage &preview);
    void showOwnCameraPreview(const QImage &preview);
    void onStreamOpened(Core::StreamSession *session);

    void setFocusedTile(std::optional<TileKey> key);
    void layoutTiles();
    void onStatsShownChanged(bool shown);
    void refreshStats();
    [[nodiscard]] QStringList statsOf(const CallTile &tile) const;
    void showTileMenu(const TileKey &key, const QPoint &globalPos);
    void onTileClicked(const TileKey &key);
    void onTileDoubleClicked(const TileKey &key);
    void setTilePoppedOut(const TileKey &key, bool out);

    void popOut(bool fullScreen);
    void toggleFullScreen();
    void applyChrome();
    void toggleCollapsedByHand();
    void onNonVideoShownChanged(bool shown);
    void updatePresence();
    void requestAvatar(CallTile &tile);
    void onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap);

    [[nodiscard]] QString nameOf(Core::Snowflake userId) const;
    [[nodiscard]] QString callSummary(const QStringList &participantNames, int videoCount) const;
    [[nodiscard]] Discord::StreamKey streamKeyFor(Core::Snowflake userId) const;
    [[nodiscard]] bool isOwnShare(const TileKey &key) const { return key == TileKey::streamOf(accountId); }
    [[nodiscard]] bool isOwnCamera(const TileKey &key) const { return key == TileKey::personOf(accountId); }
    [[nodiscard]] bool ownShareShown() const;
    [[nodiscard]] bool ownCameraShown() const;
    [[nodiscard]] int rank(const CallTile &tile) const;
    [[nodiscard]] QString nonVideoSettingsKey() const;

    QPointer<Core::Audio::VoiceManager> voice;
    QPointer<Core::StreamWatcher> watcher;
    QPointer<Core::StreamBroadcaster> broadcaster;
    QPointer<Core::CameraSender> camera;
    QPointer<Core::CallVideo> video;
    Core::ImageManager *images;
    Core::Snowflake accountId;
    Core::Snowflake channel;
    QString title;
    NameResolver names;
    AvatarResolver avatars;
    AvatarRequestTracker<TileKey> avatarTracker;

    std::map<TileKey, std::unique_ptr<CallTile>> tiles;
    std::optional<TileKey> focused;
    // The click that started watching a stream, so that the second click of a double click
    // on it is not also taken as a wish to shrink it again.
    std::optional<TileKey> watchClickTile;
    QElapsedTimer watchClickTimer;

    QPointer<QWidget> dock;
    bool videoPresent = false;
    bool poppedOut = false;
    bool collapsed = false;
    bool collapsedByHand = false;
    bool dockAfterFullScreen = false;

    CallControlsBar *controls;
    QWidget *stage;
    QTimer statsTimer;
};

} // namespace UI
} // namespace Acheron
