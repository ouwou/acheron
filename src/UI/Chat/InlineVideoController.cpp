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

void InlineVideoController::onPlayerPositionChanged(const QString &key)
{
    auto it = rows.find(key);
    if (it == rows.end() || it->kind != MediaKind::Audio)
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

InlineVideoController::~InlineVideoController() = default;

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

InlineVideoController::Target InlineVideoController::targetFor(const ChatLayout::ResolvedLayout &resolved, const ChatLayout::HitRegion &region) const
{
    using Kind = ChatLayout::HitRegion::Kind;
    Target target;

    if (!Core::Media::isSupported())
        return target;

    if (region.kind == Kind::AttachmentVideo || region.kind == Kind::AttachmentAudio) {
        if (region.index < 0 || region.index >= resolved.ctx.attachments.size())
            return target;

        const bool audio = region.kind == Kind::AttachmentAudio;
        const AttachmentData &att = resolved.ctx.attachments[region.index];
        if (audio ? !att.isAudio : !att.isVideo)
            return target;

        if (att.isSpoiler && resolved.ctx.model && !resolved.ctx.model->isSpoilerRevealed(att.id))
            return target;

        target.kind = audio ? MediaKind::Audio : MediaKind::Video;
        target.key = Core::Media::attachmentKey(att.id, target.kind);
        target.url = att.originalUrl.isEmpty() ? att.proxyUrl : att.originalUrl;
        target.rect = region.rect;
        target.attachmentId = att.id;
        target.voiceMessage = att.isVoiceMessage;
        target.durationMs = att.durationMs;
        return target;
    }

    if (region.kind == Kind::EmbedVideoThumbnail) {
        if (region.index < 0 || region.index >= resolved.ctx.embeds.size())
            return target;

        const EmbedData &embed = resolved.ctx.embeds[region.index];
        if (!embed.videoPlayable)
            return target;

        target.key = Core::Media::embedKey(resolved.ctx.messageId, region.index);
        target.url = embed.videoUrl;
        target.rect = region.rect;
    }

    return target;
}

Core::Media::Player *InlineVideoController::playerFor(const QString &key) const
{
    return pool->find(key);
}

QSize InlineVideoController::decodeSize(const QRect &rect) const
{
    return rect.size() * view->viewport()->devicePixelRatioF();
}

QRect InlineVideoController::rectForKey(const QModelIndex &index, const QString &key) const
{
    auto resolved = ChatLayout::resolveLayout(view, index);
    for (const auto &hit : resolved.layout.hitRegions) {
        const Target candidate = targetFor(resolved, hit);
        if (candidate.isValid() && candidate.key == key)
            return candidate.rect;
    }
    return QRect();
}

void InlineVideoController::rememberRow(const Target &target, const QModelIndex &index)
{
    if (!index.isValid())
        return;

    Row &row = rows[target.key];
    row.index = QPersistentModelIndex(index);
    row.rect = target.rect.translated(-view->visualRect(index).topLeft());
    row.rectUnresolvable = false;
    row.attachmentId = target.attachmentId;
    row.kind = target.kind;
    row.voiceMessage = target.voiceMessage;
    row.durationMs = target.durationMs;
}

VideoControls::State InlineVideoController::controlState(const Core::Media::Player *player, const QString &key,
                                                         bool expanded) const
{
    auto state = VideoControls::stateFor(player, expanded);

    auto it = rows.constFind(key);
    if (it != rows.constEnd()) {
        state.audioOnly = it->kind == MediaKind::Audio;
        state.voiceMessage = it->voiceMessage;
        if (state.durationMs == 0)
            state.durationMs = it->durationMs;
    }

    return state;
}

VideoControls::State InlineVideoController::controlState(const Core::Media::Player *player, const QString &key) const
{
    return controlState(player, key, volumeExpanded && hoveredKey == key);
}

VideoControls::State InlineVideoController::audioBarState(const QString &key, bool voiceMessage, qint64 fallbackDurationMs) const
{
    const auto *player = pool->find(key);

    if (player && player->state() == Core::Media::Player::State::Error)
        player = nullptr;

    auto state = controlState(player, key);
    state.audioOnly = true;
    state.voiceMessage = voiceMessage;
    if (state.durationMs == 0)
        state.durationMs = fallbackDurationMs;
    return state;
}

Core::Media::Player *InlineVideoController::ensurePlayer(const Target &target, const QModelIndex &index)
{
    if (!target.isValid())
        return nullptr;

    const bool existed = pool->find(target.key) != nullptr;
    auto *player = pool->acquire(target.key, target.url);
    if (!player)
        return nullptr;

    rememberRow(target, index);
    player->setTargetSize(decodeSize(target.rect));

    if (!existed)
        player->play();

    return player;
}

void InlineVideoController::press(const Target &target, const QPoint &pos)
{
    auto *player = pool->find(target.key);
    if (!player)
        return;

    const auto state = controlState(player, target.key);
    const auto layout = VideoControls::calculate(target.rect, state);

    if (VideoControls::beginDrag(player, layout, state, pos, drag, target.key))
        refreshRow(target.key);
}

void InlineVideoController::release(const Target &target, const QModelIndex &index, const QPoint &pos)
{
    auto *player = pool->find(target.key);

    if (!player) {
        ensurePlayer(target, index);
        refreshRow(target.key);
        return;
    }

    pool->touch(target.key);
    player->setTargetSize(decodeSize(target.rect));
    rememberRow(target, index);

    if (player->state() == Core::Media::Player::State::Error) {
        player->open(target.url);
        player->play();
        refreshRow(target.key);
        return;
    }

    const auto state = controlState(player, target.key);
    const auto layout = VideoControls::calculate(target.rect, state);

    if (VideoControls::handleRelease(player, layout, state, pos) == VideoControls::ReleaseResult::ToggleFullscreen)
        openFullscreen(target);

    refreshRow(target.key);
}

void InlineVideoController::updateDrag(const QPoint &pos)
{
    Core::Media::Player *player = pool->find(drag.key);
    if (!player) {
        drag.end();
        return;
    }

    auto it = rows.constFind(drag.key);
    if (it == rows.constEnd() || !it.value().index.isValid())
        return;

    const QModelIndex index = it.value().index;
    QRect rect = it.value().rect;
    if (!rect.isNull()) {
        rect.translate(view->visualRect(index).topLeft());
    } else {
        rect = rectForKey(index, drag.key);
        if (rect.isNull())
            return;

        Target refreshed;
        refreshed.key = drag.key;
        refreshed.rect = rect;
        refreshed.attachmentId = it.value().attachmentId;
        rememberRow(refreshed, index);
    }

    const auto state = controlState(player, drag.key, true);
    const auto layout = VideoControls::calculate(rect, state);

    VideoControls::applyDrag(player, layout, pos, drag);

    refreshRow(drag.key);
}

void InlineVideoController::updateHover(const Target &target, const QPoint &pos)
{
    setHovered(target.isValid() ? target.key : QString());

    bool expanded = false;
    if (target.isValid()) {
        if (auto *player = pool->find(target.key)) {
            const auto state = controlState(player, target.key);
            const auto layout = VideoControls::calculate(target.rect, state);
            const QRect zone = VideoControls::volumeHoverZone(layout);
            expanded = !zone.isNull() && zone.contains(pos);
        }
    }

    if (expanded == volumeExpanded)
        return;

    volumeExpanded = expanded;
    refreshRow(target.isValid() ? target.key : hoveredKey);
}

void InlineVideoController::refreshHoverAt(const QPoint &viewportPos)
{
    if (drag.active())
        return;

    const QModelIndex index = view->indexAt(viewportPos);
    auto resolved = ChatLayout::resolveLayout(view, index);
    const auto region = ChatLayout::hitTest(resolved, viewportPos);

    updateHover(region ? targetFor(resolved, *region) : Target(), viewportPos);
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

    const QModelIndex index = it->index;
    const QRect row = view->visualRect(index);

    if (it->rect.isNull() && !it->rectUnresolvable) {
        const QRect rect = rectForKey(index, key);
        if (rect.isNull())
            it->rectUnresolvable = true;
        else
            it->rect = rect.translated(-row.topLeft());
    }

    if (it->rect.isNull())
        view->viewport()->update(row);
    else
        view->viewport()->update(it->rect.translated(row.topLeft()));
}

QString InlineVideoController::fastPathKey(const QModelIndex &index, QRect *rectOut) const
{
    if (damageRect.isNull())
        return QString();

    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        if (it->rect.isNull() || it->index != index)
            continue;

        const QRect rect = it->rect.translated(view->visualRect(index).topLeft());
        if (rect.contains(damageRect)) {
            *rectOut = rect;
            return it.key();
        }
    }

    return QString();
}

void InlineVideoController::invalidateRects()
{
    for (Row &row : rows) {
        row.rect = QRect();
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

        row.rect = QRect();
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
    if (it == rows.constEnd() || !it.value().attachmentId.isValid())
        return;

    const auto *player = pool->find(key);
    if (!player || !player->hasVideo())
        return;

    if (auto *model = qobject_cast<ChatModel *>(view->model()))
        model->setVideoNativeSize(it.value().attachmentId, player->nativeSize());
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

void InlineVideoController::openFullscreen(const Target &target)
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
