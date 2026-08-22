#pragma once

#include <QHash>
#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QRect>
#include <QString>

#include <functional>
#include <optional>

#include "Core/Media/Player.hpp"
#include "UI/Chat/MediaTarget.hpp"
#include "UI/Chat/VideoControls.hpp"

namespace Acheron {

namespace Core {
namespace Media {
class PlayerPool;
} // namespace Media
} // namespace Core

namespace UI {

class ChatView;
class VideoFullscreenWindow;

class InlineVideoController : public QObject
{
    Q_OBJECT
public:
    explicit InlineVideoController(ChatView *view);
    ~InlineVideoController() override;

    using UrlRefresher = std::function<void(const QUrl &stale, std::function<void(const QUrl &fresh)>)>;
    void setUrlRefresher(UrlRefresher refresher) { urlRefresher = std::move(refresher); }

    void press(const MediaTarget &target, const QPoint &pos);
    void release(const MediaTarget &target, const QModelIndex &index, const QPoint &pos);
    [[nodiscard]] bool dragging() const { return drag.active(); }
    void updateDrag(const QPoint &pos);
    void endDrag() { drag.end(); }

    void updateHover(const MediaTarget &target, const QPoint &pos);
    void refreshHoverAt(const QPoint &viewportPos);
    void clearHover() { updateHover(MediaTarget(), QPoint()); }

    void attachModel(QAbstractItemModel *model);
    void reset();
    void invalidateRects();

    void setPaintDamage(const QRect &damage) { damageRect = damage; }
    [[nodiscard]] std::optional<MediaTarget> surfaceCoveringDamage(const QModelIndex &index) const;
    [[nodiscard]] Core::Media::Player *playerFor(const QString &key) const;
    [[nodiscard]] bool isHovered(const QString &key) const { return hoveredKey == key; }
    [[nodiscard]] bool volumeExpandedFor(const QString &key) const
    {
        return volumeExpanded && hoveredKey == key;
    }

private:
    struct Row
    {
        QPersistentModelIndex index;
        // target rect relative to row visual rect
        MediaTarget target;
        bool rectUnresolvable = false;
        qint64 paintedPositionBucket = -1;
    };

    [[nodiscard]] VideoControls::Session sessionFor(const MediaTarget &target, bool forceExpanded = false) const;
    [[nodiscard]] std::optional<MediaTarget> targetForKey(const QModelIndex &index, const QString &key) const;
    [[nodiscard]] std::optional<MediaTarget> resolvedTarget(Row &row, const QString &key);
    [[nodiscard]] std::optional<MediaTarget> resolvedTarget(const QString &key);
    [[nodiscard]] QSize decodeSize(const QRect &rect) const;
    void rememberRow(const MediaTarget &target, const QModelIndex &index);
    void handleRelease(Core::Media::Player *player, const MediaTarget &target, const QPoint &pos);
    void onPlayerPositionChanged(const QString &key);
    void refreshRow(const QString &key);
    void setHovered(const QString &key);
    void invalidateRows(int firstRow, int lastRow);
    void dropRemovedRows();
    void adoptNativeSize(const QString &key);
    void onPlayerReleased(const QString &key);
    void onPlayerStateChanged(const QString &key);
    void openFullscreen(const MediaTarget &target);
    void startPlayback(const MediaTarget &target);
    void retryPlayback(const MediaTarget &target);
    void withPlayableUrl(const QUrl &url, bool force, std::function<void(const QUrl &)> play);

    ChatView *view = nullptr;
    Core::Media::PlayerPool *pool = nullptr;
    VideoFullscreenWindow *fullscreen = nullptr;
    QPointer<QAbstractItemModel> boundModel;

    UrlRefresher urlRefresher;
    QHash<QUrl, QUrl> refreshedUrls;
    QHash<QUrl, QList<std::function<void(const QUrl &)>>> pendingRefreshes;

    QHash<QString, Row> rows;
    QRect damageRect;
    QString hoveredKey;
    VideoControls::Drag drag;
    bool volumeExpanded = false;
};

} // namespace UI
} // namespace Acheron
