#pragma once

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QSet>
#include <QTimer>

#include <optional>

#include "Core/AnimatedImageCache.hpp"
#include "Core/Emoji/PickerEmoji.hpp"
#include "Core/Snowflake.hpp"

namespace Acheron {
namespace UI {

class PaintedImages;

class EmojiGridView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    static constexpr int Columns = 9;
    static constexpr int CellPx = 40;
    static constexpr int EmojiPx = 32;

    struct Cell
    {
        int section = -1;
        int index = -1;

        [[nodiscard]] bool isValid() const { return section >= 0 && index >= 0; }
        bool operator==(const Cell &other) const { return section == other.section && index == other.index; }
    };

    explicit EmojiGridView(QWidget *parent = nullptr);

    void setImageSources(Core::ImageManager *imageManager, Core::AnimatedImageCache *animatedCache, Core::Snowflake accountId);
    void setAnimationEnabled(bool enabled);

    void setSections(const QList<Core::PickerSection> &sections);
    void setEmptyText(const QString &text);

    void setCollapsedSections(const QSet<QString> &sectionIds);
    [[nodiscard]] const QSet<QString> &collapsedSections() const { return collapsed; }

    void scrollToSection(int section);
    [[nodiscard]] int topSection() const;

    void activateFirst();
    void moveActive(int columnDelta, int rowDelta);
    [[nodiscard]] bool activeIsFirst() const;
    [[nodiscard]] const Core::PickerEmoji *activeEmoji() const;

    [[nodiscard]] QSize sizeHint() const override;

signals:
    void emojiClicked(const Acheron::Core::PickerEmoji &emoji, Qt::KeyboardModifiers modifiers);
    void activeEmojiChanged();
    void topSectionChanged(int section);
    void collapsedSectionsChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct SectionGeometry
    {
        int top = 0;
        int headerHeight = 0;
        int rows = 0;

        [[nodiscard]] int cellsTop() const { return top + headerHeight; }
        [[nodiscard]] int bottom() const { return cellsTop() + rows * CellPx; }
    };

    struct Hit
    {
        Cell cell;
        int headerSection = -1;
    };

    void relayout();
    [[nodiscard]] int scrollY() const;
    [[nodiscard]] int sectionAt(int contentY) const;
    [[nodiscard]] QRect cellRect(const Cell &cell) const;
    [[nodiscard]] Hit hitTest(const QPoint &viewportPos) const;
    [[nodiscard]] const Core::PickerEmoji *emojiAt(const Cell &cell) const;
    [[nodiscard]] bool hasCells(int section) const;
    [[nodiscard]] int adjacentSectionWithCells(int section, int step) const;

    void setActive(const Cell &cell);
    void ensureVisible(const Cell &cell);
    void activateUnderCursor();
    void toggleCollapsed(int section);
    void notifyTopSection();

    template <typename Visitor>
    void forEachCellIn(const QRect &viewportRect, Visitor visit) const;

    void paintCell(QPainter &painter, const Cell &cell, const QRect &rect);
    void paintHeader(QPainter &painter, int section, int y);
    void paintStickyHeader(QPainter &painter);
    [[nodiscard]] int stickyHeaderY(int section) const;

    struct Animation
    {
        QUrl url;
        int paintedFrame = -1;
    };

    [[nodiscard]] bool animating() const;
    [[nodiscard]] Core::AnimatedFramesPtr framesFor(const Core::PickerEmoji &emoji);
    [[nodiscard]] QPixmap currentFrame(const Core::PickerEmoji &emoji);
    void advanceAnimations();
    void releaseOffscreenFrames();
    void releaseFramesExcept(const QSet<Core::Snowflake> &keptEmojiIds);
    void onScrollSettled();

    PaintedImages *images;
    Core::AnimatedImageCache *animatedCache = nullptr;
    QMetaObject::Connection framesReadyConnection;

    QList<Core::PickerSection> shownSections;
    QList<SectionGeometry> geometry;
    QSet<QString> collapsed;
    QString emptyText;

    Cell active;
    Cell pressedCell;
    int pressedHeader = -1;
    int reportedTopSection = -1;
    std::optional<QPoint> lastMousePos;
    bool keyboardNavigating = false;

    bool animationEnabled = true;
    bool scrolling = false;
    QTimer scrollSettleTimer;
    QTimer animationTimer;
    QElapsedTimer animationClock;
    QHash<Core::Snowflake, Animation> animations;
};

} // namespace UI
} // namespace Acheron
