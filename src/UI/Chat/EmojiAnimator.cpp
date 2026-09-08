#include "UI/Chat/EmojiAnimator.hpp"

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

EmojiAnimator::EmojiAnimator(ChatView *chatView)
    : QObject(chatView), view(chatView)
{
    clock.start();
    timer.setSingleShot(true);
    timer.setTimerType(Qt::CoarseTimer);
    connect(&timer, &QTimer::timeout, this, &EmojiAnimator::onTick);

    focused = qGuiApp->applicationState() == Qt::ApplicationActive;
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        focused = state == Qt::ApplicationActive;
        updateRunning();
    });
}

void EmojiAnimator::setCache(Core::AnimatedImageCache *animatedCache)
{
    cache = animatedCache;
    connect(cache, &Core::AnimatedImageCache::framesReady, this, &EmojiAnimator::onFramesReady);
    updateRunning();
}

void EmojiAnimator::setEnabled(bool value)
{
    if (enabled == value)
        return;
    enabled = value;
    if (!enabled)
        visible.clear();
    updateRunning();
    view->viewport()->update();
}

void EmojiAnimator::setViewVisible(bool value)
{
    if (viewVisible == value)
        return;
    viewVisible = value;
    updateRunning();
}

void EmojiAnimator::attachModel(QAbstractItemModel *model)
{
    if (boundModel == model)
        return;

    if (boundModel)
        disconnect(boundModel, nullptr, this, nullptr);

    reset();
    boundModel = model;

    if (!model)
        return;

    connect(model, &QAbstractItemModel::modelReset, this, &EmojiAnimator::reset);
    connect(model, &QAbstractItemModel::rowsInserted, this, &EmojiAnimator::invalidateRects);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &EmojiAnimator::invalidateRects);
    connect(model, &QAbstractItemModel::layoutChanged, this, &EmojiAnimator::invalidateRects);
    connect(model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight) {
                for (int row = topLeft.row(); row <= bottomRight.row(); ++row)
                    rows.remove(row);
            });
}

void EmojiAnimator::reset()
{
    rows.clear();
    visible.clear();
}

bool EmojiAnimator::intersectsPaintDamage(const QRect &rowRect) const
{
    return damage.isEmpty() || damage.intersects(rowRect);
}

void EmojiAnimator::beginRow(int row, const QRect &rowRect, const QRect &bodyTextRect)
{
    const QPoint origin = rowRect.topLeft();
    recording = Recording{ row, origin, Row{ {}, bodyTextRect.translated(-origin) } };
}

void EmojiAnimator::endRow()
{
    const auto finished = std::exchange(recording, std::nullopt);
    if (!finished)
        return;

    if (finished->data.instances.isEmpty())
        rows.remove(finished->row);
    else
        rows.insert(finished->row, finished->data);
}

QPixmap EmojiAnimator::frame(const QUrl &url, const QSize &size, Core::Snowflake accountId, const QRect &viewportRect)
{
    if (!enabled || !cache)
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
        recording->data.instances.append({ key, rectInRow, inBody });
    }

    return it->frames ? it->frames->frames[it->frameIndex] : QPixmap();
}

std::optional<EmojiAnimator::BodyRepaint> EmojiAnimator::bodyOnlyRepaint(int row, const QRect &rowRect) const
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

bool EmojiAnimator::running() const
{
    return enabled && focused && viewVisible && cache;
}

qint64 EmojiAnimator::now() const
{
    return animatedMs + (runningSince ? clock.elapsed() - *runningSince : 0);
}

int EmojiAnimator::currentFrame(const Core::AnimatedFrames &frames) const
{
    return frames.frameAt(frames.isStatic() ? 0 : now() % frames.loopMs());
}

void EmojiAnimator::updateRunning()
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

void EmojiAnimator::scheduleTick(int delayMs)
{
    if (!running())
        return;
    if (timer.isActive() && timer.remainingTime() <= delayMs)
        return;
    timer.start(delayMs);
}

void EmojiAnimator::pruneOffscreenRows()
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

QRegion EmojiAnimator::regionFor(const QSet<Key> &keys) const
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

void EmojiAnimator::onTick()
{
    if (!running())
        return;

    pruneOffscreenRows();

    const qint64 t = now();
    QSet<Key> changed;
    qint64 nextDelay = std::numeric_limits<qint64>::max();

    for (auto it = visible.begin(); it != visible.end(); ++it) {
        VisibleEmoji &entry = it.value();
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

void EmojiAnimator::onFramesReady(const QUrl &url, const QSize &size, const Core::AnimatedFramesPtr &frames)
{
    const Key key{ url, size };
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
