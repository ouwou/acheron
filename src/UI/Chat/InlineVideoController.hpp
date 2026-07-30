#pragma once

#include <QHash>
#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QUrl>

#include "Core/Media/Player.hpp"
#include "Core/Snowflake.hpp"
#include "UI/Chat/ChatLayout.hpp"
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

    struct Target
    {
        QString key;
        QUrl url;
        QRect rect;
        Core::Snowflake attachmentId;
        Core::Media::MediaKind kind = Core::Media::MediaKind::Video;
        bool voiceMessage = false;
        qint64 durationMs = 0;

        [[nodiscard]] bool isValid() const { return !url.isEmpty(); }
    };

    [[nodiscard]] Target targetFor(const ChatLayout::ResolvedLayout &resolved, const ChatLayout::HitRegion &region) const;

    void press(const Target &target, const QPoint &pos);
    void release(const Target &target, const QModelIndex &index, const QPoint &pos);
    [[nodiscard]] bool dragging() const { return drag.active(); }
    void updateDrag(const QPoint &pos);
    void endDrag() { drag.end(); }

    void updateHover(const Target &target, const QPoint &pos);
    void refreshHoverAt(const QPoint &viewportPos);
    void clearHover() { updateHover(Target(), QPoint()); }

    void attachModel(QAbstractItemModel *model);
    void reset();
    void invalidateRects();

    void setPaintDamage(const QRect &damage) { damageRect = damage; }
    [[nodiscard]] QRect paintDamage() const { return damageRect; }
    [[nodiscard]] QString fastPathKey(const QModelIndex &index, QRect *rectOut) const;
    [[nodiscard]] Core::Media::Player *playerFor(const QString &key) const;
    [[nodiscard]] VideoControls::State controlState(const Core::Media::Player *player, const QString &key) const;
    [[nodiscard]] VideoControls::State audioBarState(const QString &key, bool voiceMessage, qint64 fallbackDurationMs) const;
    [[nodiscard]] bool isHovered(const QString &key) const { return hoveredKey == key; }

private:
    struct Row
    {
        QPersistentModelIndex index;
        // media rect relative to row visual rect
        QRect rect;
        bool rectUnresolvable = false;
        Core::Snowflake attachmentId;
        Core::Media::MediaKind kind = Core::Media::MediaKind::Video;
        bool voiceMessage = false;
        qint64 durationMs = 0;
        qint64 paintedPositionBucket = -1;
    };

    [[nodiscard]] VideoControls::State controlState(const Core::Media::Player *player, const QString &key, bool expanded) const;
    [[nodiscard]] QSize decodeSize(const QRect &rect) const;
    [[nodiscard]] QRect rectForKey(const QModelIndex &index, const QString &key) const;
    void rememberRow(const Target &target, const QModelIndex &index);
    Core::Media::Player *ensurePlayer(const Target &target, const QModelIndex &index);
    void onPlayerPositionChanged(const QString &key);
    void refreshRow(const QString &key);
    void setHovered(const QString &key);
    void invalidateRows(int firstRow, int lastRow);
    void dropRemovedRows();
    void adoptNativeSize(const QString &key);
    void onPlayerReleased(const QString &key);
    void onPlayerStateChanged(const QString &key);
    void openFullscreen(const Target &target);

    ChatView *view = nullptr;
    Core::Media::PlayerPool *pool = nullptr;
    VideoFullscreenWindow *fullscreen = nullptr;
    QPointer<QAbstractItemModel> boundModel;

    QHash<QString, Row> rows;
    QRect damageRect;
    QString hoveredKey;
    VideoControls::Drag drag;
    bool volumeExpanded = false;
};

} // namespace UI
} // namespace Acheron
