#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QRect>
#include <QRegion>
#include <QSet>
#include <QTimer>

#include <optional>

#include "Core/AnimatedImageCache.hpp"
#include "Core/Snowflake.hpp"

class QAbstractItemModel;

namespace Acheron {
namespace UI {

class ChatView;

class EmojiAnimator : public QObject
{
    Q_OBJECT
public:
    explicit EmojiAnimator(ChatView *view);

    void setCache(Core::AnimatedImageCache *cache);
    void setEnabled(bool enabled);
    [[nodiscard]] bool isEnabled() const { return enabled; }
    void setViewVisible(bool visible);

    void attachModel(QAbstractItemModel *model);
    void reset();
    void invalidateRects() { rows.clear(); }
    void setPaintDamage(const QRegion &region) { damage = region; }
    [[nodiscard]] bool intersectsPaintDamage(const QRect &rowRect) const;

    void beginRow(int row, const QRect &rowRect, const QRect &bodyTextRect);
    void endRow();

    [[nodiscard]] QPixmap frame(const QUrl &url, const QSize &size, Core::Snowflake accountId, const QRect &viewportRect);

    struct BodyRepaint
    {
        QRect textRect;
        QRect damage;
    };

    [[nodiscard]] std::optional<BodyRepaint> bodyOnlyRepaint(int row, const QRect &rowRect) const;

private:
    using Key = Core::ImageRequestKey;

    struct Instance
    {
        Key key;
        QRect rectInRow;
        bool inBody = false;
    };

    struct Row
    {
        QList<Instance> instances;
        QRect bodyTextRectInRow;
    };

    struct Recording
    {
        int row;
        QPoint origin;
        Row data;
    };

    struct VisibleEmoji
    {
        Core::AnimatedFramesPtr frames;
        int frameIndex = 0;
    };

    [[nodiscard]] bool running() const;
    [[nodiscard]] qint64 now() const;
    [[nodiscard]] int currentFrame(const Core::AnimatedFrames &frames) const;
    void updateRunning();
    void scheduleTick(int delayMs);
    void onTick();
    void onFramesReady(const QUrl &url, const QSize &size, const Core::AnimatedFramesPtr &frames);
    void pruneOffscreenRows();
    [[nodiscard]] QRegion regionFor(const QSet<Key> &keys) const;

    ChatView *view;
    Core::AnimatedImageCache *cache = nullptr;
    QPointer<QAbstractItemModel> boundModel;

    QHash<int, Row> rows;
    QHash<Key, VisibleEmoji> visible;
    QRegion damage;
    std::optional<Recording> recording;

    QElapsedTimer clock;
    qint64 animatedMs = 0;
    std::optional<qint64> runningSince;
    QTimer timer;

    bool enabled = true;
    bool focused = true;
    bool viewVisible = false;
    bool wasRunning = false;
};

} // namespace UI
} // namespace Acheron
