#include "UI/Chat/InlineVideoController.hpp"

#include "Core/Media/PlayerPool.hpp"
#include "UI/Chat/ChatModel.hpp"
#include "UI/Chat/ChatView.hpp"
#include "UI/VideoFullscreenWindow.hpp"

namespace Acheron {
namespace UI {

using Core::Media::MediaKind;

namespace {
constexpr qint64 AudioRepaintBucketMs = 250;
} // namespace

InlineVideoController::InlineVideoController(ChatView *chatView)
    : QObject(chatView), view(chatView)
{
    pool = new Core::Media::PlayerPool(this);

    connect(pool, &Core::Media::PlayerPool::frameReady, this, &InlineVideoController::refreshRow);
    connect(pool, &Core::Media::PlayerPool::playerPositionChanged, this, &InlineVideoController::onPlayerPositionChanged);
    connect(pool, &Core::Media::PlayerPool::playerStateChanged, this, &InlineVideoController::onPlayerStateChanged);
    connect(pool, &Core::Media::PlayerPool::playerReleased, this, &InlineVideoController::onPlayerReleased);
}

InlineVideoController::~InlineVideoController() = default;

void InlineVideoController::onPlayerPositionChanged(const QString &key)
{
    auto it = rows.find(key);
    if (it == rows.end() || it->target.kind != MediaKind::Audio)
        return;

    const auto *player = pool->find(key);
    if (!player)
        return;

    const qint64 bucket = player->position() / AudioRepaintBucketMs;
    if (bucket == it->paintedPositionBucket)
        return;

    it->paintedPositionBucket = bucket;
    refreshRow(key);
}

void InlineVideoController::attachModel(QAbstractItemModel *model)
{
    if (boundModel == model)
        return;

    if (boundModel)
        disconnect(boundModel, nullptr, this, nullptr);

    reset();
    boundModel = model;

    if (!model)
        return;

    connect(model, &QAbstractItemModel::modelReset, this, &InlineVideoController::reset);
    connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { dropRemovedRows(); });
    connect(model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight) {
                invalidateRows(topLeft.row(), bottomRight.row());
            });
}

void InlineVideoController::reset()
{
    pool->clear();
    rows.clear();
    drag.end();
    hoveredKey.clear();
    volumeExpanded = false;
}

Core::Media::Player *InlineVideoController::playerFor(const QString &key) const
{
    return pool->find(key);
}

VideoControls::Session InlineVideoController::sessionFor(const MediaTarget &target, bool forceExpanded) const
{
    return VideoControls::sessionFor(pool->find(target.key),
                                     target.rect,
                                     target.info(),
                                     forceExpanded || volumeExpandedFor(target.key));
}

QSize InlineVideoController::decodeSize(const QRect &rect) const
{
    return rect.size() * view->viewport()->devicePixelRatioF();
}

std::optional<MediaTarget> InlineVideoController::targetForKey(const QModelIndex &index, const QString &key) const
{
    auto resolved = ChatLayout::resolveLayout(view, index);
    for (const auto &hit : resolved.layout.hitRegions) {
        MediaTarget candidate = MediaTargets::forRegion(resolved, hit);
        if (candidate.isValid() && candidate.key == key)
            return candidate;
    }
    return std::nullopt;
}

std::optional<MediaTarget> InlineVideoController::resolvedTarget(Row &row, const QString &key)
{
    if (!row.index.isValid())
        return std::nullopt;

    const QPoint rowOrigin = view->visualRect(row.index).topLeft();

    if (row.target.rect.isNull() && !row.rectUnresolvable) {
        if (auto fresh = targetForKey(row.index, key)) {
            row.target = *fresh;
            row.target.rect.translate(-rowOrigin);
        } else {
            row.rectUnresolvable = true;
        }
    }

    if (row.target.rect.isNull())
        return std::nullopt;

    MediaTarget target = row.target;
    target.rect.translate(rowOrigin);
    return target;
}

std::optional<MediaTarget> InlineVideoController::resolvedTarget(const QString &key)
{
    auto it = rows.find(key);
    if (it == rows.end())
        return std::nullopt;
    return resolvedTarget(*it, key);
}

void InlineVideoController::rememberRow(const MediaTarget &target, const QModelIndex &index)
{
    if (!index.isValid())
        return;

    Row &row = rows[target.key];
    row.index = QPersistentModelIndex(index);
    row.target = target;
    row.target.rect = target.rect.translated(-view->visualRect(index).topLeft());
    row.rectUnresolvable = false;
}

void InlineVideoController::press(const MediaTarget &target, const QPoint &pos)
{
    auto *player = pool->find(target.key);
    if (!player || target.spoilered)
        return;

    const auto session = sessionFor(target);

    if (VideoControls::beginDrag(player, session.layout, session.state, pos, drag, target.key))
        refreshRow(target.key);
}

void InlineVideoController::release(const MediaTarget &target, const QModelIndex &index, const QPoint &pos)
{
    if (!target.isValid() || target.spoilered)
        return;

    auto *player = pool->find(target.key);
    const bool starting = !player;
    if (starting)
        player = pool->acquire(target.key, target.url);
    else
        pool->touch(target.key);
    if (!player)
        return;

    rememberRow(target, index);
    player->setTargetSize(decodeSize(target.rect));

    if (starting)
        player->play();
    else
        handleRelease(player, target, pos);

    refreshRow(target.key);
}

void InlineVideoController::handleRelease(Core::Media::Player *player, const MediaTarget &target, const QPoint &pos)
{
    const auto session = sessionFor(target);

    if (session.state.status == VideoControls::Status::Failed) {
        if (VideoControls::hitTest(session.layout, pos, session.state) != VideoControls::Hit::None) {
            player->open(target.url);
            player->play();
        }
        return;
    }

    if (VideoControls::handleRelease(player, session.layout, session.state, pos) ==
        VideoControls::ReleaseResult::ToggleFullscreen)
        openFullscreen(target);
}

void InlineVideoController::updateDrag(const QPoint &pos)
{
    Core::Media::Player *player = pool->find(drag.key);
    if (!player) {
        drag.end();
        return;
    }

    const auto target = resolvedTarget(drag.key);
    if (!target)
        return;

    const auto session = sessionFor(*target, true);
    VideoControls::applyDrag(player, session.layout, pos, drag);

    refreshRow(drag.key);
}

void InlineVideoController::updateHover(const MediaTarget &target, const QPoint &pos)
{
    setHovered(target.isValid() ? target.key : QString());

    const bool expanded = target.isValid() && pool->find(target.key) &&
                          VideoControls::volumeHoverZone(sessionFor(target).layout).contains(pos);

    if (expanded == volumeExpanded)
        return;

    volumeExpanded = expanded;
    refreshRow(hoveredKey);
}

void InlineVideoController::refreshHoverAt(const QPoint &viewportPos)
{
    if (drag.active())
        return;

    const QModelIndex index = view->indexAt(viewportPos);
    auto resolved = ChatLayout::resolveLayout(view, index);
    const auto region = ChatLayout::hitTest(resolved, viewportPos);

    updateHover(region ? MediaTargets::forRegion(resolved, *region) : MediaTarget(), viewportPos);
}

void InlineVideoController::setHovered(const QString &key)
{
    if (hoveredKey == key)
        return;

    const QString previous = hoveredKey;
    hoveredKey = key;
    if (key.isEmpty())
        volumeExpanded = false;

    if (!previous.isEmpty())
        refreshRow(previous);
    if (!hoveredKey.isEmpty())
        refreshRow(hoveredKey);
}

void InlineVideoController::refreshRow(const QString &key)
{
    auto it = rows.find(key);
    if (it == rows.end())
        return;

    if (!it->index.isValid()) {
        view->viewport()->update();
        return;
    }

    if (const auto target = resolvedTarget(*it, key))
        view->viewport()->update(target->rect);
    else
        view->viewport()->update(view->visualRect(it->index));
}

std::optional<MediaTarget> InlineVideoController::surfaceCoveringDamage(const QModelIndex &index) const
{
    if (damageRect.isNull())
        return std::nullopt;

    const QPoint rowOrigin = view->visualRect(index).topLeft();

    for (const Row &row : rows) {
        if (row.index != index || row.target.rect.isNull())
            continue;

        MediaTarget target = row.target;
        target.rect.translate(rowOrigin);
        if (target.rect.contains(damageRect))
            return target;
    }

    return std::nullopt;
}

void InlineVideoController::invalidateRects()
{
    for (Row &row : rows) {
        row.target.rect = QRect();
        row.rectUnresolvable = false;
    }
}

void InlineVideoController::invalidateRows(int firstRow, int lastRow)
{
    for (Row &row : rows) {
        if (!row.index.isValid())
            continue;

        const int rowIndex = row.index.row();
        if (rowIndex < firstRow || rowIndex > lastRow)
            continue;

        row.target.rect = QRect();
        row.rectUnresolvable = false;
    }
}

void InlineVideoController::dropRemovedRows()
{
    QStringList gone;
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        if (!it.value().index.isValid())
            gone.append(it.key());
    }

    for (const QString &key : gone) {
        pool->release(key);
        rows.remove(key);
    }
}

void InlineVideoController::adoptNativeSize(const QString &key)
{
    auto it = rows.constFind(key);
    if (it == rows.constEnd() || !it.value().target.attachmentId.isValid())
        return;

    const auto *player = pool->find(key);
    if (!player || !player->hasVideo())
        return;

    if (auto *model = qobject_cast<ChatModel *>(view->model()))
        model->setVideoNativeSize(it.value().target.attachmentId, player->nativeSize());
}

void InlineVideoController::onPlayerStateChanged(const QString &key)
{
    adoptNativeSize(key);
    refreshRow(key);
}

void InlineVideoController::onPlayerReleased(const QString &key)
{
    if (fullscreen && fullscreen->isVisible() && fullscreen->mediaKey() == key)
        fullscreen->close();

    if (drag.key == key)
        drag.end();

    auto it = rows.constFind(key);
    if (it != rows.constEnd() && it.value().index.isValid())
        refreshRow(key);

    rows.remove(key);
}

void InlineVideoController::openFullscreen(const MediaTarget &target)
{
    auto *player = pool->find(target.key);
    if (!player)
        return;

    if (!fullscreen) {
        fullscreen = new VideoFullscreenWindow(view);
        connect(fullscreen, &VideoFullscreenWindow::closed, this, [this] {
            pool->setPinned(QString());
            view->viewport()->update();
        });
    }

    pool->setPinned(target.key);
    fullscreen->showPlayer(player, target.key, decodeSize(target.rect));
}

} // namespace UI
} // namespace Acheron
