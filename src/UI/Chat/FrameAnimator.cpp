#include "UI/Chat/FrameAnimator.hpp"

#include <QAbstractItemModel>
#include <QGuiApplication>

#include <algorithm>
#include <limits>
#include <utility>

#include "UI/Chat/ChatView.hpp"

namespace Acheron {
namespace UI {

namespace {
constexpr int MinTickMs = 15;
constexpr int MaxTickMs = 1000;
} // namespace

FrameAnimator::FrameAnimator(ChatView *chatView)
    : QObject(chatView), view(chatView)
{
    clock.start();
    timer.setSingleShot(true);
    timer.setTimerType(Qt::CoarseTimer);
    connect(&timer, &QTimer::timeout, this, &FrameAnimator::onTick);

    focused = qGuiApp->applicationState() == Qt::ApplicationActive;
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        focused = state == Qt::ApplicationActive;
        updateRunning();
    });
}

void FrameAnimator::setCache(Core::AnimatedImageCache *animatedCache)
{
    cache = animatedCache;
    connect(cache, &Core::AnimatedImageCache::framesReady, this, &FrameAnimator::onFramesReady);
    updateRunning();
}

void FrameAnimator::setEmojiEnabled(bool value)
{
    applySetting(animateEmoji, value);
}

void FrameAnimator::setStickersEnabled(bool value)
{
    applySetting(animateStickers, value);
}

void FrameAnimator::applySetting(bool &setting, bool value)
{
    if (setting == value)
        return;
    setting = value;
    if (!setting)
        visible.clear();
    updateRunning();
    view->viewport()->update();
}

void FrameAnimator::setViewVisible(bool value)
{
    if (viewVisible == value)
        return;
    viewVisible = value;
    updateRunning();
}

void FrameAnimator::attachModel(QAbstractItemModel *model)
{
    if (boundModel == model)
        return;

    if (boundModel)
        disconnect(boundModel, nullptr, this, nullptr);

    reset();
    boundModel = model;

    if (!model)
        return;

    connect(model, &QAbstractItemModel::modelReset, this, &FrameAnimator::reset);
    connect(model, &QAbstractItemModel::rowsInserted, this, &FrameAnimator::invalidateRects);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &FrameAnimator::invalidateRects);
    connect(model, &QAbstractItemModel::layoutChanged, this, &FrameAnimator::invalidateRects);
    connect(model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight) {
                for (int row = topLeft.row(); row <= bottomRight.row(); ++row)
                    rows.remove(row);
            });
}

void FrameAnimator::reset()
{
    rows.clear();
    visible.clear();
    awaitedDecodes.clear();
}

bool FrameAnimator::intersectsPaintDamage(const QRect &rowRect) const
{
    return damage.isEmpty() || damage.intersects(rowRect);
}

void FrameAnimator::beginRow(int row, const QRect &rowRect, const QRect &bodyTextRect)
{
    const QPoint origin = rowRect.topLeft();
    recording = Recording{ row, origin, Row{ {}, bodyTextRect.translated(-origin) } };
}

void FrameAnimator::endRow()
{
    const auto finished = std::exchange(recording, std::nullopt);
    if (!finished)
        return;

    if (finished->data.instances.isEmpty())
        rows.remove(finished->row);
    else
        rows.insert(finished->row, finished->data);
}

QPixmap FrameAnimator::frame(const QUrl &url, const QSize &size, Core::Snowflake accountId, const QRect &viewportRect, AnimatedKind kind)
{
    const bool animates = kind == AnimatedKind::Sticker ? animateStickers : animateEmoji;
    if (!animates || !cache)
        return {};

    const Key key{ url, size };
    auto it = visible.find(key);
    if (it == visible.end()) {
        const Core::AnimatedFramesPtr frames = cache->get(url, size, accountId);
        if (frames && frames->isStatic())
            return {};
        it = visible.insert(key, { frames, frames ? currentFrame(*frames) : 0 });
        if (frames)
            scheduleTick(0);
    }

    if (recording) {
        const QRect rectInRow = viewportRect.translated(-recording->origin);
        const bool inBody = recording->data.bodyTextRectInRow.contains(rectInRow);
        recording->data.instances.append({ key, rectInRow, inBody, kind });
    }

    return it->frames ? it->frames->frames[it->frameIndex] : QPixmap();
}

Core::AnimatedFramesPtr FrameAnimator::framesOnceDecoded(const QUrl &url, const QSize &size, Core::Snowflake accountId)
{
    if (!cache)
        return nullptr;

    const Core::AnimatedFramesPtr frames = cache->get(url, size, accountId);
    if (!frames)
        awaitedDecodes.insert({ url, size });
    return frames;
}

std::optional<FrameAnimator::BodyRepaint> FrameAnimator::bodyOnlyRepaint(int row, const QRect &rowRect) const
{
    if (damage.isEmpty())
        return std::nullopt;

    auto it = rows.constFind(row);
    if (it == rows.constEnd())
        return std::nullopt;

    const QRegion rowDamage = damage.intersected(rowRect);
    if (rowDamage.isEmpty())
        return std::nullopt;

    QRegion covered;
    for (const Instance &instance : it->instances) {
        if (instance.inBody)
            covered += instance.rectInRow.translated(rowRect.topLeft());
    }
    if (covered.isEmpty() || !rowDamage.subtracted(covered).isEmpty())
        return std::nullopt;

    return BodyRepaint{ it->bodyTextRectInRow.translated(rowRect.topLeft()), rowDamage.boundingRect() };
}

std::optional<QList<FrameAnimator::StickerRepaint>> FrameAnimator::stickerOnlyRepaint(int row, const QRect &rowRect) const
{
    if (damage.isEmpty())
        return std::nullopt;

    auto it = rows.constFind(row);
    if (it == rows.constEnd())
        return std::nullopt;

    const QRegion rowDamage = damage.intersected(rowRect);
    if (rowDamage.isEmpty())
        return std::nullopt;

    QRegion covered;
    QList<StickerRepaint> repaints;
    for (const Instance &instance : it->instances) {
        if (instance.kind != AnimatedKind::Sticker)
            continue;

        const QRect rect = instance.rectInRow.translated(rowRect.topLeft());
        if (!rowDamage.intersects(rect))
            continue;

        const auto shown = visible.constFind(instance.key);
        if (shown == visible.constEnd() || !shown->frames)
            return std::nullopt;

        covered += rect;
        repaints.append({ rect, shown->frames->frames[shown->frameIndex] });
    }
    if (repaints.isEmpty() || !rowDamage.subtracted(covered).isEmpty())
        return std::nullopt;

    return repaints;
}

bool FrameAnimator::running() const
{
    return (animateEmoji || animateStickers) && focused && viewVisible && cache;
}

qint64 FrameAnimator::now() const
{
    return animatedMs + (runningSince ? clock.elapsed() - *runningSince : 0);
}

int FrameAnimator::currentFrame(const Core::AnimatedFrames &frames) const
{
    return frames.frameAt(frames.isStatic() ? 0 : now() % frames.loopMs());
}

void FrameAnimator::updateRunning()
{
    const bool run = running();
    if (run == wasRunning)
        return;
    wasRunning = run;

    if (run) {
        runningSince = clock.elapsed();
        scheduleTick(0);
    } else {
        animatedMs = now();
        runningSince.reset();
        timer.stop();
        pruneOffscreenRows();
    }
}

void FrameAnimator::scheduleTick(int delayMs)
{
    if (!running())
        return;
    if (timer.isActive() && timer.remainingTime() <= delayMs)
        return;
    timer.start(delayMs);
}

void FrameAnimator::pruneOffscreenRows()
{
    const QRect viewportRect = view->viewport()->rect();
    const int rowCount = view->model() ? view->model()->rowCount() : 0;

    for (auto it = rows.begin(); it != rows.end();) {
        const bool onScreen = it.key() < rowCount && view->visualRect(view->model()->index(it.key(), 0)).intersects(viewportRect);
        if (onScreen)
            ++it;
        else
            it = rows.erase(it);
    }

    QSet<Key> live;
    for (const Row &row : rows) {
        for (const Instance &instance : row.instances)
            live.insert(instance.key);
    }
    for (auto it = visible.begin(); it != visible.end();) {
        if (live.contains(it.key()))
            ++it;
        else
            it = visible.erase(it);
    }
}

QRegion FrameAnimator::regionFor(const QSet<Key> &keys) const
{
    QRegion region;
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        const QPoint origin = view->visualRect(view->model()->index(it.key(), 0)).topLeft();
        for (const Instance &instance : it->instances) {
            if (keys.contains(instance.key))
                region += instance.rectInRow.translated(origin);
        }
    }
    return region;
}

void FrameAnimator::onTick()
{
    if (!running())
        return;

    pruneOffscreenRows();

    const qint64 t = now();
    QSet<Key> changed;
    qint64 nextDelay = std::numeric_limits<qint64>::max();

    for (auto it = visible.begin(); it != visible.end(); ++it) {
        VisibleAnimation &entry = it.value();
        if (!entry.frames)
            continue;

        const qint64 phase = t % entry.frames->loopMs();
        const int index = entry.frames->frameAt(phase);
        if (index != entry.frameIndex) {
            entry.frameIndex = index;
            changed.insert(it.key());
        }
        nextDelay = std::min(nextDelay, qint64(entry.frames->frameEndMs[index]) - phase);
    }

    if (!changed.isEmpty())
        view->viewport()->update(regionFor(changed));

    if (nextDelay != std::numeric_limits<qint64>::max())
        timer.start(int(std::clamp<qint64>(nextDelay, MinTickMs, MaxTickMs)));
}

void FrameAnimator::onFramesReady(const QUrl &url, const QSize &size, const Core::AnimatedFramesPtr &frames)
{
    const Key key{ url, size };
    if (awaitedDecodes.remove(key))
        view->viewport()->update();

    auto it = visible.find(key);
    if (it == visible.end())
        return;

    if (frames->isStatic()) {
        visible.erase(it);
        return;
    }

    it->frames = frames;
    it->frameIndex = currentFrame(*frames);
    view->viewport()->update(regionFor({ key }));
    scheduleTick(0);
}

} // namespace UI
} // namespace Acheron
