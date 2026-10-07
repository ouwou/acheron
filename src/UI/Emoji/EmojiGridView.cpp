#include "UI/Emoji/EmojiGridView.hpp"

#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>

#include <algorithm>

#include "Core/Theme/Icons.hpp"
#include "Core/Theme/Manager.hpp"
#include "UI/Emoji/EmojiPainting.hpp"
#include "UI/Emoji/PaintedImages.hpp"

namespace Acheron {
namespace UI {

namespace {

constexpr int SidePadding = 8;
constexpr int HeaderHeight = 28;
constexpr int SectionGap = 8;
constexpr int HeaderIconPx = 16;
constexpr int ChevronPx = 12;
constexpr int HeaderItemGap = 6;
constexpr int CellRadius = 4;
constexpr int ScrollSettleMs = 120;
constexpr int StillsBudgetKiB = 8 * 1024;
constexpr int MinAnimationTickMs = 16;
constexpr QSize FrameSize(EmojiGridView::EmojiPx, EmojiGridView::EmojiPx);

} // namespace

EmojiGridView::EmojiGridView(QWidget *parent) : QAbstractScrollArea(parent), images(new PaintedImages(viewport(), StillsBudgetKiB))
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setFocusPolicy(Qt::NoFocus);
    viewport()->setMouseTracking(true);
    viewport()->setAutoFillBackground(false);
    verticalScrollBar()->setSingleStep(CellPx);

    scrollSettleTimer.setSingleShot(true);
    scrollSettleTimer.setInterval(ScrollSettleMs);
    connect(&scrollSettleTimer, &QTimer::timeout, this, &EmojiGridView::onScrollSettled);

    animationTimer.setSingleShot(true);
    connect(&animationTimer, &QTimer::timeout, this, &EmojiGridView::advanceAnimations);
    animationClock.start();
}

void EmojiGridView::setImageSources(Core::ImageManager *imageManager, Core::AnimatedImageCache *animatedCache, Core::Snowflake accountId)
{
    images->setSource(imageManager, accountId);
    if (this->animatedCache == animatedCache)
        return;

    QObject::disconnect(framesReadyConnection);
    releaseFramesExcept({});
    this->animatedCache = animatedCache;
    if (animatedCache)
        framesReadyConnection = connect(animatedCache, &Core::AnimatedImageCache::framesReady, this, [this]() {
            if (isVisible())
                viewport()->update();
        });
}

void EmojiGridView::setAnimationEnabled(bool enabled)
{
    animationEnabled = enabled;
}

void EmojiGridView::setSections(const QList<Core::PickerSection> &sections)
{
    shownSections = sections;
    active = {};
    pressedCell = {};
    pressedHeader = -1;
    reportedTopSection = -1;

    relayout();
    verticalScrollBar()->setValue(0);
    releaseOffscreenFrames();
    viewport()->update();
    notifyTopSection();
    emit activeEmojiChanged();
}

void EmojiGridView::setEmptyText(const QString &text)
{
    emptyText = text;
}

void EmojiGridView::setCollapsedSections(const QSet<QString> &sectionIds)
{
    collapsed = sectionIds;
    relayout();
    viewport()->update();
}

void EmojiGridView::relayout()
{
    geometry.clear();
    geometry.reserve(shownSections.size());

    int y = 0;
    for (const Core::PickerSection &section : shownSections) {
        SectionGeometry sectionGeometry;
        sectionGeometry.top = y;
        sectionGeometry.headerHeight = section.title.isEmpty() ? 0 : HeaderHeight;
        sectionGeometry.rows = collapsed.contains(section.id) ? 0 : int((section.emojis.size() + Columns - 1) / Columns);
        geometry.append(sectionGeometry);
        y = sectionGeometry.bottom() + SectionGap;
    }
    const int contentHeight = y;

    const int viewHeight = viewport()->height();
    verticalScrollBar()->setRange(0, qMax(0, contentHeight - viewHeight));
    verticalScrollBar()->setPageStep(viewHeight);
}

int EmojiGridView::scrollY() const
{
    return verticalScrollBar()->value();
}

int EmojiGridView::sectionAt(int contentY) const
{
    const auto after = std::upper_bound(geometry.cbegin(), geometry.cend(), contentY, [](int y, const SectionGeometry &sectionGeometry) { return y < sectionGeometry.top; });
    return int(after - geometry.cbegin()) - 1;
}

int EmojiGridView::topSection() const
{
    return geometry.isEmpty() ? -1 : qMax(0, sectionAt(scrollY()));
}

QRect EmojiGridView::cellRect(const Cell &cell) const
{
    const SectionGeometry &sectionGeometry = geometry.at(cell.section);
    return QRect(SidePadding + (cell.index % Columns) * CellPx, sectionGeometry.cellsTop() + (cell.index / Columns) * CellPx - scrollY(), CellPx, CellPx);
}

int EmojiGridView::stickyHeaderY(int section) const
{
    const SectionGeometry &sectionGeometry = geometry.at(section);
    return qMin(qMax(0, sectionGeometry.top - scrollY()), sectionGeometry.bottom() - scrollY() - sectionGeometry.headerHeight);
}

EmojiGridView::Hit EmojiGridView::hitTest(const QPoint &viewportPos) const
{
    Hit hit;
    if (geometry.isEmpty())
        return hit;

    const int top = topSection();
    if (geometry.at(top).headerHeight > 0) {
        const int headerY = stickyHeaderY(top);
        if (viewportPos.y() >= headerY && viewportPos.y() < headerY + HeaderHeight) {
            hit.headerSection = top;
            return hit;
        }
    }

    const int contentY = viewportPos.y() + scrollY();
    const int section = sectionAt(contentY);
    if (section < 0)
        return hit;

    const SectionGeometry &sectionGeometry = geometry.at(section);
    if (contentY < sectionGeometry.cellsTop()) {
        hit.headerSection = section;
        return hit;
    }

    const int x = viewportPos.x() - SidePadding;
    if (contentY >= sectionGeometry.bottom() || x < 0 || x >= Columns * CellPx)
        return hit;

    const int index = ((contentY - sectionGeometry.cellsTop()) / CellPx) * Columns + x / CellPx;
    if (index < shownSections.at(section).emojis.size())
        hit.cell = { section, index };
    return hit;
}

const Core::PickerEmoji *EmojiGridView::emojiAt(const Cell &cell) const
{
    if (!cell.isValid() || cell.section >= shownSections.size() || cell.index >= shownSections.at(cell.section).emojis.size())
        return nullptr;
    return &shownSections.at(cell.section).emojis.at(cell.index);
}

const Core::PickerEmoji *EmojiGridView::activeEmoji() const
{
    return emojiAt(active);
}

bool EmojiGridView::hasCells(int section) const
{
    return geometry.at(section).rows > 0;
}

int EmojiGridView::adjacentSectionWithCells(int section, int step) const
{
    for (int candidate = section + step; candidate >= 0 && candidate < geometry.size(); candidate += step) {
        if (hasCells(candidate))
            return candidate;
    }
    return -1;
}

void EmojiGridView::setActive(const Cell &cell)
{
    if (active == cell)
        return;

    if (active.isValid())
        viewport()->update(cellRect(active));
    active = cell;
    if (active.isValid())
        viewport()->update(cellRect(active));
    emit activeEmojiChanged();
}

void EmojiGridView::activateFirst()
{
    const int first = adjacentSectionWithCells(-1, 1);
    setActive(first < 0 ? Cell() : Cell{ first, 0 });
}

bool EmojiGridView::activeIsFirst() const
{
    return active.isValid() && active.index == 0 && adjacentSectionWithCells(active.section, -1) < 0;
}

void EmojiGridView::moveActive(int columnDelta, int rowDelta)
{
    keyboardNavigating = true;
    if (!active.isValid() || !hasCells(active.section)) {
        activateFirst();
        return;
    }

    const auto countIn = [this](int section) { return int(shownSections.at(section).emojis.size()); };
    const int count = countIn(active.section);
    Cell target = active;

    if (columnDelta != 0) {
        target.index += columnDelta;
        if (target.index < 0) {
            const int previous = adjacentSectionWithCells(active.section, -1);
            if (previous < 0)
                return;
            target = { previous, countIn(previous) - 1 };
        } else if (target.index >= count) {
            const int next = adjacentSectionWithCells(active.section, 1);
            if (next < 0)
                return;
            target = { next, 0 };
        }
    }

    if (rowDelta != 0) {
        const int column = active.index % Columns;
        const int row = active.index / Columns + rowDelta;
        if (row < 0) {
            const int previous = adjacentSectionWithCells(active.section, -1);
            if (previous < 0)
                return;
            const int lastRowStart = ((countIn(previous) - 1) / Columns) * Columns;
            target = { previous, qMin(lastRowStart + column, countIn(previous) - 1) };
        } else if (row > (count - 1) / Columns) {
            const int next = adjacentSectionWithCells(active.section, 1);
            if (next < 0)
                return;
            target = { next, qMin(column, countIn(next) - 1) };
        } else {
            target.index = qMin(row * Columns + column, count - 1);
        }
    }

    setActive(target);
    ensureVisible(target);
}

void EmojiGridView::ensureVisible(const Cell &cell)
{
    const SectionGeometry &sectionGeometry = geometry.at(cell.section);
    const int cellTop = sectionGeometry.cellsTop() + (cell.index / Columns) * CellPx;
    const int visibleTop = scrollY() + sectionGeometry.headerHeight;
    const int visibleBottom = scrollY() + viewport()->height();

    if (cellTop < visibleTop)
        verticalScrollBar()->setValue(cellTop - sectionGeometry.headerHeight);
    else if (cellTop + CellPx > visibleBottom)
        verticalScrollBar()->setValue(cellTop + CellPx - viewport()->height());
}

void EmojiGridView::scrollToSection(int section)
{
    if (section >= 0 && section < geometry.size())
        verticalScrollBar()->setValue(geometry.at(section).top);
}

void EmojiGridView::toggleCollapsed(int section)
{
    const QString id = shownSections.at(section).id;
    if (id.isEmpty())
        return;

    if (!collapsed.remove(id))
        collapsed.insert(id);
    if (active.section == section)
        setActive({});

    relayout();
    verticalScrollBar()->setValue(qMin(scrollY(), geometry.at(section).top));
    viewport()->update();
    emit collapsedSectionsChanged();
}

void EmojiGridView::notifyTopSection()
{
    const int top = topSection();
    if (top == reportedTopSection)
        return;
    reportedTopSection = top;
    emit topSectionChanged(top);
}

template <typename Visitor>
void EmojiGridView::forEachCellIn(const QRect &viewportRect, Visitor visit) const
{
    if (geometry.isEmpty())
        return;

    const int firstY = viewportRect.top() + scrollY();
    const int lastY = viewportRect.bottom() + scrollY();
    for (int section = qMax(0, sectionAt(firstY)); section < geometry.size(); section++) {
        const SectionGeometry &sectionGeometry = geometry.at(section);
        if (sectionGeometry.top > lastY)
            break;
        if (sectionGeometry.rows == 0 || lastY < sectionGeometry.cellsTop() || firstY >= sectionGeometry.bottom())
            continue;

        const int firstRow = qMax(0, (firstY - sectionGeometry.cellsTop()) / CellPx);
        const int lastRow = qMin(sectionGeometry.rows - 1, (lastY - sectionGeometry.cellsTop()) / CellPx);
        const int count = int(shownSections.at(section).emojis.size());
        for (int index = firstRow * Columns; index < qMin(count, (lastRow + 1) * Columns); index++)
            visit(Cell{ section, index });
    }
}

bool EmojiGridView::animating() const
{
    return animationEnabled && animatedCache && isVisible();
}

Core::AnimatedFramesPtr EmojiGridView::framesFor(const Core::PickerEmoji &emoji)
{
    Animation &animation = animations[emoji.customId];
    if (animation.url.isEmpty())
        animation.url = emojiAnimationUrl(emoji.customId, devicePixelRatioF());

    const Core::AnimatedFramesPtr frames = animatedCache->get(animation.url, FrameSize, images->accountId());
    return frames && !frames->isStatic() && frames->loopMs() > 0 ? frames : nullptr;
}

QPixmap EmojiGridView::currentFrame(const Core::PickerEmoji &emoji)
{
    if (!emoji.animated || scrolling || !animating())
        return {};

    const Core::AnimatedFramesPtr frames = framesFor(emoji);
    if (!frames)
        return {};

    const int index = frames->frameAt(animationClock.elapsed() % frames->loopMs());
    animations[emoji.customId].paintedFrame = index;
    return frames->frames.at(index);
}

void EmojiGridView::advanceAnimations()
{
    if (scrolling || !animating())
        return;

    const qint64 now = animationClock.elapsed();
    int nextTickMs = -1;
    forEachCellIn(viewport()->rect(), [&](const Cell &cell) {
        const Core::PickerEmoji &emoji = shownSections.at(cell.section).emojis.at(cell.index);
        const auto animation = animations.constFind(emoji.customId);
        const int paintedFrame = animation == animations.constEnd() ? -1 : animation->paintedFrame;
        if (paintedFrame < 0)
            return;
        const Core::AnimatedFramesPtr frames = framesFor(emoji);
        if (!frames)
            return;

        const int phase = int(now % frames->loopMs());
        const int index = frames->frameAt(phase);
        if (paintedFrame != index)
            viewport()->update(cellRect(cell));

        const int untilNextFrame = frames->frameEndMs.at(index) - phase;
        if (nextTickMs < 0 || untilNextFrame < nextTickMs)
            nextTickMs = untilNextFrame;
    });

    if (nextTickMs >= 0)
        animationTimer.start(qMax(nextTickMs, MinAnimationTickMs));
}

void EmojiGridView::releaseOffscreenFrames()
{
    QSet<Core::Snowflake> inView;
    forEachCellIn(viewport()->rect(), [&](const Cell &cell) {
        const Core::PickerEmoji &emoji = shownSections.at(cell.section).emojis.at(cell.index);
        if (emoji.animated)
            inView.insert(emoji.customId);
    });
    releaseFramesExcept(inView);
}

void EmojiGridView::releaseFramesExcept(const QSet<Core::Snowflake> &keptEmojiIds)
{
    for (auto it = animations.begin(); it != animations.end();) {
        if (keptEmojiIds.contains(it.key())) {
            ++it;
            continue;
        }
        animatedCache->discard(it->url, FrameSize);
        it = animations.erase(it);
    }
}

void EmojiGridView::paintCell(QPainter &painter, const Cell &cell, const QRect &rect)
{
    const Core::Theme::Manager &theme = Core::Theme::Manager::instance();
    const Core::PickerEmoji &emoji = shownSections.at(cell.section).emojis.at(cell.index);

    if (cell == active) {
        QColor highlight = theme.color(Core::Theme::Token::Highlight);
        highlight.setAlpha(80);
        painter.setPen(Qt::NoPen);
        painter.setBrush(highlight);
        painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), CellRadius, CellRadius);
    }

    const int inset = (CellPx - EmojiPx) / 2;
    const QRect emojiRect(rect.left() + inset, rect.top() + inset, EmojiPx, EmojiPx);
    const QPixmap frame = currentFrame(emoji);
    if (!frame.isNull()) {
        drawCentered(painter, emojiRect, frame);
        return;
    }
    if (drawEmojiStill(painter, emojiRect, emoji, *images, rect))
        return;

    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.color(Core::Theme::Token::AlternateBaseBg));
    painter.drawRoundedRect(emojiRect, CellRadius, CellRadius);
}

void EmojiGridView::paintHeader(QPainter &painter, int section, int y)
{
    using namespace Core::Theme;
    const Manager &theme = Manager::instance();
    const Core::PickerSection &shown = shownSections.at(section);
    const qreal dpr = devicePixelRatioF();

    const QRect rect(0, y, viewport()->width(), HeaderHeight);
    painter.fillRect(rect, theme.color(Token::BaseBg));

    int x = SidePadding;
    const auto chevron = collapsed.contains(shown.id) ? Icons::Name::ChevronRight : Icons::Name::ChevronDown;
    painter.drawPixmap(x, y + (HeaderHeight - ChevronPx) / 2, Icons::pixmap(chevron, ChevronPx, Token::PlaceholderText, dpr));
    x += ChevronPx + HeaderItemGap;

    if (shown.guildIconUrl.isValid()) {
        const QRect iconRect(x, y + (HeaderHeight - HeaderIconPx) / 2, HeaderIconPx, HeaderIconPx);
        const QPixmap icon = images->pixmap(shown.guildIconUrl, HeaderIconPx, rect);
        if (!icon.isNull())
            drawCentered(painter, iconRect, icon);
        x += HeaderIconPx + HeaderItemGap;
    }

    QFont font = theme.font(FontRole::Ui);
    if (font.pointSizeF() > 0)
        font.setPointSizeF(font.pointSizeF() * 0.85);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(theme.color(Token::PlaceholderText));

    const QRect textRect(x, y, rect.right() - SidePadding - x, HeaderHeight);
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, painter.fontMetrics().elidedText(shown.title, Qt::ElideRight, textRect.width()));
}

void EmojiGridView::paintStickyHeader(QPainter &painter)
{
    const int top = topSection();
    if (top >= 0 && geometry.at(top).headerHeight > 0)
        paintHeader(painter, top, stickyHeaderY(top));
}

void EmojiGridView::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (shownSections.isEmpty()) {
        painter.setPen(Core::Theme::Manager::instance().color(Core::Theme::Token::PlaceholderText));
        painter.drawText(viewport()->rect(), Qt::AlignCenter, emptyText);
        return;
    }

    const QRect damage = event->rect();
    forEachCellIn(damage, [&](const Cell &cell) { paintCell(painter, cell, cellRect(cell)); });

    const int top = topSection();
    for (int section = qMax(0, sectionAt(damage.top() + scrollY())); section < geometry.size(); section++) {
        const SectionGeometry &sectionGeometry = geometry.at(section);
        const int headerY = sectionGeometry.top - scrollY();
        if (headerY > damage.bottom())
            break;
        if (section != top && sectionGeometry.headerHeight > 0)
            paintHeader(painter, section, headerY);
    }
    paintStickyHeader(painter);

    if (!animations.isEmpty() && !animationTimer.isActive())
        animationTimer.start(MinAnimationTickMs);
}

void EmojiGridView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    relayout();
}

void EmojiGridView::scrollContentsBy(int, int)
{
    scrolling = true;
    scrollSettleTimer.start();
    viewport()->update();
    notifyTopSection();
}

void EmojiGridView::onScrollSettled()
{
    scrolling = false;
    releaseOffscreenFrames();
    activateUnderCursor();
    viewport()->update();
}

void EmojiGridView::activateUnderCursor()
{
    if (keyboardNavigating)
        return;

    const QPoint pos = viewport()->mapFromGlobal(QCursor::pos());
    if (!viewport()->rect().contains(pos))
        return;
    const Hit hit = hitTest(pos);
    if (hit.cell.isValid())
        setActive(hit.cell);
}

void EmojiGridView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint globalPos = viewport()->mapToGlobal(event->pos());
    if (lastMousePos && *lastMousePos == globalPos)
        return;
    lastMousePos = globalPos;
    keyboardNavigating = false;

    const Hit hit = hitTest(event->pos());
    viewport()->setCursor(hit.cell.isValid() || hit.headerSection >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (!scrolling && hit.cell.isValid())
        setActive(hit.cell);
}

void EmojiGridView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const Hit hit = hitTest(event->pos());
    pressedCell = hit.cell;
    pressedHeader = hit.headerSection;
}

void EmojiGridView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;

    const Hit hit = hitTest(event->pos());
    const bool onPressedHeader = hit.headerSection >= 0 && hit.headerSection == pressedHeader;
    const bool onPressedCell = hit.cell.isValid() && hit.cell == pressedCell;
    pressedCell = {};
    pressedHeader = -1;

    if (onPressedHeader) {
        toggleCollapsed(hit.headerSection);
    } else if (onPressedCell) {
        const Core::PickerEmoji emoji = *emojiAt(hit.cell);
        emit emojiClicked(emoji, event->modifiers());
    }
}

void EmojiGridView::showEvent(QShowEvent *event)
{
    QAbstractScrollArea::showEvent(event);
    lastMousePos.reset();
    keyboardNavigating = false;
}

void EmojiGridView::hideEvent(QHideEvent *event)
{
    animationTimer.stop();
    scrollSettleTimer.stop();
    scrolling = false;
    releaseFramesExcept({});
    images->clear();
    QAbstractScrollArea::hideEvent(event);
}

QSize EmojiGridView::sizeHint() const
{
    constexpr int VisibleRows = 8;
    return QSize(2 * SidePadding + Columns * CellPx + verticalScrollBar()->sizeHint().width(), VisibleRows * CellPx);
}

} // namespace UI
} // namespace Acheron
