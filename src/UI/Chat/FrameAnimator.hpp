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

enum class AnimatedKind {
    Emoji,
    Sticker,
};

class FrameAnimator : public QObject
{
    Q_OBJECT
public:
    explicit FrameAnimator(ChatView *view);

    void setCache(Core::AnimatedImageCache *cache);
    void setEmojiEnabled(bool enabled);
    [[nodiscard]] bool emojiEnabled() const { return animateEmoji; }
    void setStickersEnabled(bool enabled);
    [[nodiscard]] bool stickersEnabled() const { return animateStickers; }
    void setViewVisible(bool visible);

    void attachModel(QAbstractItemModel *model);
    void reset();
    void invalidateRects() { rows.clear(); }
    void setPaintDamage(const QRegion &region) { damage = region; }
    [[nodiscard]] bool intersectsPaintDamage(const QRect &rowRect) const;

    void beginRow(int row, const QRect &rowRect, const QRect &bodyTextRect);
    void endRow();

    [[nodiscard]] QPixmap frame(const QUrl &url, const QSize &size, Core::Snowflake accountId, const QRect &viewportRect,
                                AnimatedKind kind = AnimatedKind::Emoji);
    // null while decoding, and the view repaints once that finishes
    [[nodiscard]] Core::AnimatedFramesPtr framesOnceDecoded(const QUrl &url, const QSize &size, Core::Snowflake accountId);

    struct BodyRepaint
    {
        QRect textRect;
        QRect damage;
    };

    [[nodiscard]] std::optional<BodyRepaint> bodyOnlyRepaint(int row, const QRect &rowRect) const;

    struct StickerRepaint
    {
        QRect rect;
        QPixmap frame;
    };

    [[nodiscard]] std::optional<QList<StickerRepaint>> stickerOnlyRepaint(int row, const QRect &rowRect) const;

private:
    using Key = Core::ImageRequestKey;

    struct Instance
    {
        Key key;
        QRect rectInRow;
        bool inBody = false;
        AnimatedKind kind = AnimatedKind::Emoji;
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

    struct VisibleAnimation
    {
        Core::AnimatedFramesPtr frames;
        int frameIndex = 0;
    };

    void applySetting(bool &setting, bool value);
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
    QHash<Key, VisibleAnimation> visible;
    QSet<Key> awaitedDecodes;
    QRegion damage;
    std::optional<Recording> recording;

    QElapsedTimer clock;
    qint64 animatedMs = 0;
    std::optional<qint64> runningSince;
    QTimer timer;

    bool animateEmoji = true;
    bool animateStickers = true;
    bool focused = true;
    bool viewVisible = false;
    bool wasRunning = false;
};

} // namespace UI
} // namespace Acheron
