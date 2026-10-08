#include "UI/Gifs/FavoriteGifGrid.hpp"

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
#include <QUrl>

#include <algorithm>
#include <utility>

#include "Core/ImageManager.hpp"
#include "Core/Theme/Manager.hpp"
#include "UI/Chat/ChatLayout.hpp"
#include "UI/Chat/GifPainting.hpp"
#include "UI/Chat/GifPlayback.hpp"
#include "UI/Emoji/PaintedImages.hpp"

namespace Acheron {
namespace UI {

namespace {

constexpr int StillBudgetKiB = 8 * 1024;
constexpr int ScrollStepPx = 48;
constexpr int PlaceholderTextMargin = 8;
constexpr int MaxStillFetchesAtOnce = 4;
constexpr int ScrollSettledAfterMs = 150;
constexpr int RetryStillsAfterMs = int(Core::ImageManager::FailedFetchRetryMs) + 2000;

Core::FavoriteGifCandidate asCandidate(const Proto::FavoriteGif &gif)
{
    return { gif.url, gif.src, {}, gif.format, QSize(int(gif.width), int(gif.height)) };
}

bool showTheSameTiles(const QList<Proto::FavoriteGif> &current, const QList<Proto::FavoriteGif> &next)
{
    return std::equal(current.cbegin(), current.cend(), next.cbegin(), next.cend(), [](const Proto::FavoriteGif &a, const Proto::FavoriteGif &b) {
        return a.url == b.url && a.src == b.src && a.width == b.width && a.height == b.height;
    });
}

} // namespace

void FavoriteGifModel::setGifs(const QList<Proto::FavoriteGif> &shownGifs)
{
    beginResetModel();
    shown = shownGifs;
    endResetModel();
}

int FavoriteGifModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(shown.size());
}

QVariant FavoriteGifModel::data(const QModelIndex &index, int role) const
{
    const bool describesTile = index.isValid() && index.row() < shown.size() && (role == Qt::DisplayRole || role == Qt::ToolTipRole);
    return describesTile ? QVariant(shown[index.row()].url) : QVariant();
}

FavoriteGifGrid::FavoriteGifGrid(QWidget *parent)
    : QAbstractItemView(parent), gifModel(new FavoriteGifModel(this)), stills(new PaintedImages(viewport(), StillBudgetKiB))
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    verticalScrollBar()->setSingleStep(ScrollStepPx);

    setModel(gifModel);
    playback = new GifPlayback(this, [this] { return account; });
    playback->attachModel(gifModel);

    scrollSettledTimer.setSingleShot(true);
    scrollSettledTimer.setInterval(ScrollSettledAfterMs);
    connect(&scrollSettledTimer, &QTimer::timeout, this, &FavoriteGifGrid::fetchStillsHeldBack);

    retryStillsTimer.setSingleShot(true);
    retryStillsTimer.setInterval(RetryStillsAfterMs);
    connect(&retryStillsTimer, &QTimer::timeout, viewport(), qOverload<>(&QWidget::update));

    connect(stills, &PaintedImages::fetchFinished, this, &FavoriteGifGrid::onStillFetchFinished);
}

QPixmap FavoriteGifGrid::stillFor(const Proto::FavoriteGif &gif, const QRect &rect)
{
    const QUrl url = Core::FavoriteGifRules::stillServedByDiscord(gif);
    const QSize size = GifMasonry::stillSizeFor(QSize(int(gif.width), int(gif.height)), rect.width());

    const bool mayFetch = !scrollSettledTimer.isActive() && stills->fetchesInFlight() < MaxStillFetchesAtOnce;
    if (mayFetch)
        return stills->pixmap(url, size, rect);

    const QPixmap still = stills->pixmapWithoutFetching(url, size);
    if (still.isNull() && url.isValid())
        stillsHeldBack = true;
    return still;
}

void FavoriteGifGrid::fetchStillsHeldBack()
{
    if (std::exchange(stillsHeldBack, false))
        viewport()->update();
}

void FavoriteGifGrid::onStillFetchFinished(bool fetched)
{
    if (!fetched)
        retryStillsTimer.start();
    fetchStillsHeldBack();
}

void FavoriteGifGrid::setSources(Core::ImageManager *imageManager, Core::Snowflake accountId, Core::FavoriteGifs *favorites)
{
    account = accountId;
    stills->setSource(imageManager, accountId);
    playback->setImageManager(imageManager);
    playback->setFavorites(favorites);
}

void FavoriteGifGrid::setGifs(const QList<Proto::FavoriteGif> &gifs)
{
    if (showTheSameTiles(gifModel->gifs(), gifs))
        return;

    gifModel->setGifs(gifs);
    arrangeTiles();
    viewport()->update();
}

void FavoriteGifGrid::setColumns(int columnCount)
{
    if (std::exchange(columns, columnCount) == columnCount)
        return;

    const QScrollBar *scrollBar = verticalScrollBar();
    const double scrolledFraction = scrollBar->maximum() > 0 ? double(scrollBar->value()) / scrollBar->maximum() : 0.0;
    arrangeTiles();
    verticalScrollBar()->setValue(qRound(scrolledFraction * verticalScrollBar()->maximum()));
    viewport()->update();
}

void FavoriteGifGrid::setEmptyText(const QString &text)
{
    emptyText = text;
    viewport()->update();
}

void FavoriteGifGrid::arrangeTiles()
{
    QList<QSize> naturalSizes;
    naturalSizes.reserve(gifModel->gifs().size());
    for (const Proto::FavoriteGif &gif : gifModel->gifs())
        naturalSizes.append(QSize(int(gif.width), int(gif.height)));

    arrangement = GifMasonry::arrange(naturalSizes, viewport()->width(), columns);
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setRange(0, qMax(0, arrangement.contentHeight - viewport()->height()));
    playback->viewportMoved();
}

QRect FavoriteGifGrid::visualRect(const QModelIndex &index) const
{
    if (!index.isValid() || index.row() >= arrangement.tiles.size())
        return {};
    return arrangement.tiles[index.row()].translated(0, -verticalOffset());
}

void FavoriteGifGrid::scrollTo(const QModelIndex &index, ScrollHint)
{
    const QRect rect = visualRect(index);
    if (rect.isEmpty())
        return;

    if (rect.top() < 0)
        verticalScrollBar()->setValue(verticalOffset() + rect.top() - arrangement.gutter);
    else if (rect.bottom() >= viewport()->height())
        verticalScrollBar()->setValue(verticalOffset() + rect.bottom() + 1 - viewport()->height() + arrangement.gutter);
}

QModelIndex FavoriteGifGrid::indexAt(const QPoint &viewportPos) const
{
    const QPoint contentPos = viewportPos + QPoint(0, verticalOffset());
    for (int row = 0; row < arrangement.tiles.size(); ++row) {
        if (arrangement.tiles[row].contains(contentPos))
            return gifModel->index(row);
    }
    return {};
}

QModelIndex FavoriteGifGrid::moveCursor(CursorAction, Qt::KeyboardModifiers)
{
    return currentIndex();
}

int FavoriteGifGrid::horizontalOffset() const
{
    return 0;
}

int FavoriteGifGrid::verticalOffset() const
{
    return verticalScrollBar()->value();
}

bool FavoriteGifGrid::isIndexHidden(const QModelIndex &) const
{
    return false;
}

void FavoriteGifGrid::setSelection(const QRect &, QItemSelectionModel::SelectionFlags) {}

QRegion FavoriteGifGrid::visualRegionForSelection(const QItemSelection &) const
{
    return {};
}

void FavoriteGifGrid::paintEvent(QPaintEvent *event)
{
    using namespace Core::Theme;
    const Manager &theme = Manager::instance();

    QPainter painter(viewport());
    painter.setFont(theme.font(FontRole::Ui));

    if (gifModel->gifs().isEmpty()) {
        painter.setPen(theme.color(Token::PlaceholderText));
        painter.drawText(viewport()->rect().adjusted(GifMasonry::RoomiestGutter, 0, -GifMasonry::RoomiestGutter, 0), Qt::AlignCenter | Qt::TextWordWrap, emptyText);
        return;
    }

    playback->setPaintDamage(event->region());
    for (int row = 0; row < arrangement.tiles.size(); ++row) {
        const QRect rect = arrangement.tiles[row].translated(0, -verticalOffset());
        if (event->region().intersects(rect))
            paintTile(painter, row, rect);
    }
    playback->setPaintDamage(QRegion());
}

void FavoriteGifGrid::paintTile(QPainter &painter, int row, const QRect &rect)
{
    const QModelIndex index = gifModel->index(row);
    const Proto::FavoriteGif &gif = gifModel->gifs()[row];

    if (const auto clips = playback->clipOnlyRepaint(index, rect)) {
        for (const auto &clip : *clips) {
            GifPainting::drawClipFrame(&painter, clip.rect, clip.frame, clip.fit);
            if (clip.badge)
                GifPainting::drawBadge(&painter, clip.rect, painter.font());
            GifPainting::drawStar(&painter, clip.rect, clip.star);
        }
        return;
    }

    playback->beginRow(index, rect);
    const QUrl clipUrl = Core::FavoriteGifRules::clipServedByDiscord(gif);
    const bool servedByDiscord = clipUrl.isValid();
    const auto shown = playback->show(GifKeys::favoriteTile(row), clipUrl, rect, ClipFit::Crop, asCandidate(gif));

    const QPixmap still = shown.frame.isNull() ? stillFor(gif, rect) : QPixmap();
    if (!shown.frame.isNull())
        GifPainting::drawClipFrame(&painter, rect, shown.frame, ClipFit::Crop);
    else if (!still.isNull())
        ChatLayout::drawCroppedPixmap(&painter, rect, still);
    else
        painter.fillRect(rect, Core::Theme::Manager::instance().color(Core::Theme::Token::AlternateBaseBg));

    if (!servedByDiscord)
        paintHostNotPreviewed(painter, gif, rect);
    if (shown.badge)
        GifPainting::drawBadge(&painter, rect, painter.font());
    GifPainting::drawStar(&painter, rect, shown.star);
    playback->endRow();
}

void FavoriteGifGrid::paintHostNotPreviewed(QPainter &painter, const Proto::FavoriteGif &gif, const QRect &rect)
{
    painter.setPen(Core::Theme::Manager::instance().color(Core::Theme::Token::PlaceholderText));
    const QRect textRect = rect.adjusted(PlaceholderTextMargin, PlaceholderTextMargin, -PlaceholderTextMargin, -PlaceholderTextMargin);
    painter.drawText(textRect, Qt::AlignCenter | Qt::TextWrapAnywhere, tr("Preview not shown\n%1").arg(QUrl(gif.src).host()));
}

void FavoriteGifGrid::resizeEvent(QResizeEvent *event)
{
    QAbstractItemView::resizeEvent(event);
    arrangeTiles();
}

void FavoriteGifGrid::scrollContentsBy(int dx, int dy)
{
    Q_UNUSED(dx);
    Q_UNUSED(dy);
    viewport()->update();
    playback->viewportMoved();
    scrollSettledTimer.start();
}

void FavoriteGifGrid::mouseMoveEvent(QMouseEvent *event)
{
    playback->updateHover(event->pos());
    const bool clickable = indexAt(event->pos()).isValid();
    viewport()->setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void FavoriteGifGrid::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;

    pressedStar = playback->isOverStar(event->pos());
    pressedRow = indexAt(event->pos()).row();
}

void FavoriteGifGrid::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;

    const int row = std::exchange(pressedRow, -1);
    if (std::exchange(pressedStar, false)) {
        playback->toggleFavoriteAt(event->pos());
        return;
    }

    if (row >= 0 && row == indexAt(event->pos()).row())
        emit gifChosen(gifModel->gifs()[row].url);
}

void FavoriteGifGrid::leaveEvent(QEvent *event)
{
    QAbstractItemView::leaveEvent(event);
    playback->clearHover();
}

void FavoriteGifGrid::showEvent(QShowEvent *event)
{
    QAbstractItemView::showEvent(event);
    playback->setViewVisible(true);
}

void FavoriteGifGrid::hideEvent(QHideEvent *event)
{
    playback->setViewVisible(false);
    playback->reset();
    stills->clear();
    scrollSettledTimer.stop();
    retryStillsTimer.stop();
    stillsHeldBack = false;
    QAbstractItemView::hideEvent(event);
}

} // namespace UI
} // namespace Acheron
