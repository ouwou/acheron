#include "UI/Chat/GifPlayback.hpp"

#include <QAbstractItemModel>
#include <QGuiApplication>

#include <algorithm>
#include <utility>

#include "UI/Chat/ChatModel.hpp"
#include "UI/Chat/ChatView.hpp"

namespace Acheron {
namespace UI {

namespace {

constexpr int StartAfterSettledMs = 120;
constexpr int PruneIntervalMs = 100;
constexpr int ClosePausedAfterMs = 30 * 1000;
constexpr double VisibleFractionToPlay = 0.6;
constexpr qint64 BytesPerMiB = 1024 * 1024;

const QSize AsDownloaded;

double visibleFraction(const QRect &rect, const QRect &viewport)
{
    if (rect.isEmpty())
        return 0.0;
    const QRect seen = rect.intersected(viewport);
    return double(seen.width()) * seen.height() / (double(rect.width()) * rect.height());
}

Qt::AspectRatioMode aspectModeFor(ClipFit fit)
{
    return fit == ClipFit::Crop ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio;
}

} // namespace

GifPlayMode gifPlayModeFromSetting(const QString &value)
{
    if (value == QLatin1String("hover"))
        return GifPlayMode::OnHover;
    if (value == QLatin1String("never"))
        return GifPlayMode::Never;
    return GifPlayMode::WhenFocused;
}

GifPlayback::GifPlayback(ChatView *chatView)
    : QObject(chatView), view(chatView), player(new Core::Media::ClipPlayer(this))
{
    connect(player, &Core::Media::ClipPlayer::frameChanged, this, &GifPlayback::onFrameChanged);
    connect(player, &Core::Media::ClipPlayer::clipFailed, this, &GifPlayback::onClipFailed);
    player->setMemoryLimitForNewClips(clipMemoryLimitMiB * BytesPerMiB);

    startTimer.setSingleShot(true);
    startTimer.setInterval(StartAfterSettledMs);
    connect(&startTimer, &QTimer::timeout, this, [this] { reconcile(Starts::Allowed); });

    pruneTimer.setSingleShot(true);
    pruneTimer.setInterval(PruneIntervalMs);
    connect(&pruneTimer, &QTimer::timeout, this, [this] { reconcile(Starts::None); });

    pausedTooLongTimer.setSingleShot(true);
    pausedTooLongTimer.setInterval(ClosePausedAfterMs);
    connect(&pausedTooLongTimer, &QTimer::timeout, this, &GifPlayback::closeClipsPausedTooLong);

    focused = qGuiApp->applicationState() == Qt::ApplicationActive;
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        const bool nowFocused = state == Qt::ApplicationActive;
        if (std::exchange(focused, nowFocused) != nowFocused)
            reconcileAndRepaint();
    });
}

void GifPlayback::setImageManager(Core::ImageManager *manager)
{
    if (imageManager == manager)
        return;

    if (imageManager)
        disconnect(imageManager, nullptr, this, nullptr);
    imageManager = manager;
    if (manager) {
        connect(manager, &Core::ImageManager::rawDownloadReady, this, &GifPlayback::onDownloadReady);
        connect(manager, &Core::ImageManager::imageUnavailable, this, &GifPlayback::onDownloadFailed);
    }
    reconcileAndRepaint();
}

void GifPlayback::setPlayMode(GifPlayMode playMode)
{
    if (std::exchange(mode, playMode) == playMode)
        return;

    if (mode == GifPlayMode::Never)
        reset();
    reconcile(Starts::Allowed);
    view->viewport()->update();
}

void GifPlayback::setMaxPlayingAtOnce(int count)
{
    const int bounded = std::clamp(count, FewestPlayingAtOnce, MostPlayingAtOnce);
    if (std::exchange(maxPlayingAtOnce, bounded) != bounded)
        reconcile(Starts::Allowed);
}

void GifPlayback::setClipMemoryLimitMiB(int mib)
{
    const int bounded = std::clamp(mib, ClipMemoryLimit.min, ClipMemoryLimit.max);
    if (std::exchange(clipMemoryLimitMiB, bounded) == bounded)
        return;

    player->setMemoryLimitForNewClips(bounded * BytesPerMiB);
    unplayable.clear();
    reconcileAndRepaint();
}

void GifPlayback::setViewVisible(bool visible)
{
    if (std::exchange(viewVisible, visible) != visible)
        reconcileAndRepaint();
}

void GifPlayback::attachModel(QAbstractItemModel *model)
{
    if (boundModel == model)
        return;

    if (boundModel)
        disconnect(boundModel, nullptr, this, nullptr);

    reset();
    boundModel = model;

    if (!model)
        return;

    connect(model, &QAbstractItemModel::modelReset, this, &GifPlayback::reset);
    connect(model, &QAbstractItemModel::rowsInserted, this, &GifPlayback::scheduleReconcile);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &GifPlayback::scheduleReconcile);
    connect(model, &QAbstractItemModel::layoutChanged, this, &GifPlayback::scheduleReconcile);
    connect(model, &QAbstractItemModel::dataChanged, this, &GifPlayback::scheduleReconcile);
}

void GifPlayback::reset()
{
    player->closeAll();
    surfaces.clear();
    openClips.clear();
    keyOfClip.clear();
    wanted.clear();
    undecided.clear();
    unplayable.clear();
    hoveredKey.reset();
    startTimer.stop();
    pruneTimer.stop();
    pausedTooLongTimer.stop();
}

void GifPlayback::viewportMoved()
{
    scheduleReconcile();
}

void GifPlayback::updateHover(const QPoint &viewportPos)
{
    cursorPos = viewportPos;
    if (hoveredKey != keyUnderCursor())
        reconcile(Starts::Allowed);
}

void GifPlayback::clearHover()
{
    cursorPos.reset();
    if (hoveredKey)
        reconcile(Starts::Allowed);
}

std::optional<GifKey> GifPlayback::keyUnderCursor() const
{
    if (!cursorPos)
        return std::nullopt;

    for (auto it = surfaces.constBegin(); it != surfaces.constEnd(); ++it) {
        if (viewportRect(*it).contains(*cursorPos))
            return it.key();
    }
    return std::nullopt;
}

void GifPlayback::beginRow(const QModelIndex &index, const QRect &rowRect)
{
    recording = Recording{ index, rowRect.topLeft(), {} };
}

void GifPlayback::endRow()
{
    const auto finished = std::exchange(recording, std::nullopt);
    if (!finished)
        return;

    bool removedAny = false;
    for (auto it = surfaces.begin(); it != surfaces.end();) {
        const bool noLongerPainted = it->index == finished->index && !finished->keys.contains(it.key());
        if (noLongerPainted) {
            it = surfaces.erase(it);
            removedAny = true;
        } else {
            ++it;
        }
    }

    if (removedAny)
        scheduleReconcile();
}

GifPlayback::Shown GifPlayback::show(const GifKey &key, const QUrl &animatedUrl, const QRect &rect, ClipFit fit)
{
    if (mode == GifPlayMode::Never)
        return { QImage(), true };

    if (recording) {
        recording->keys.insert(key);

        const QRect rectInRow = rect.translated(-recording->origin);
        const auto known = surfaces.constFind(key);
        const bool unchanged = known != surfaces.constEnd() && known->index == recording->index && known->rectInRow == rectInRow &&
                               known->url == animatedUrl && known->fit == fit;
        if (!unchanged) {
            if (known == surfaces.constEnd())
                undecided.insert(key);
            else if (known->url != animatedUrl)
                unplayable.remove(key);
            surfaces.insert(key, { QPersistentModelIndex(recording->index), rectInRow, animatedUrl, fit });
            scheduleReconcile();
        }
    }

    Shown shown;
    const auto open = openClips.constFind(key);
    if (open != openClips.constEnd())
        shown.frame = player->frame(open->id);
    shown.badge = !unplayable.contains(key) && !expectedToPlay(key);
    return shown;
}

std::optional<QList<GifPlayback::Repaint>> GifPlayback::clipOnlyRepaint(const QModelIndex &index, const QRect &rowRect) const
{
    if (damage.isEmpty() || openClips.isEmpty())
        return std::nullopt;

    const QRegion rowDamage = damage.intersected(rowRect);
    if (rowDamage.isEmpty())
        return std::nullopt;

    QRegion covered;
    QList<Repaint> repaints;
    for (auto it = surfaces.constBegin(); it != surfaces.constEnd(); ++it) {
        if (it->index != index)
            continue;

        const QRect rect = it->rectInRow.translated(rowRect.topLeft());
        if (!rowDamage.intersects(rect))
            continue;

        const auto open = openClips.constFind(it.key());
        const QImage frame = open != openClips.constEnd() ? player->frame(open->id) : QImage();
        if (frame.isNull())
            return std::nullopt;

        covered += rect;
        repaints.append({ rect, frame, it->fit, !expectedToPlay(it.key()) });
    }

    if (repaints.isEmpty() || !rowDamage.subtracted(covered).isEmpty())
        return std::nullopt;
    return repaints;
}

bool GifPlayback::gateOpen() const
{
    return mode != GifPlayMode::Never && focused && viewVisible && imageManager;
}

bool GifPlayback::expectedToPlay(const GifKey &key) const
{
    if (wanted.contains(key))
        return true;
    return undecided.contains(key) && mode == GifPlayMode::WhenFocused && gateOpen();
}

QRect GifPlayback::viewportRect(const Surface &surface) const
{
    return surface.rectInRow.translated(view->visualRect(surface.index).topLeft());
}

void GifPlayback::scheduleReconcile()
{
    startTimer.start();
    if (!pruneTimer.isActive())
        pruneTimer.start();
}

void GifPlayback::dropSurfacesOutOfView()
{
    const QRect viewport = view->viewport()->rect();
    for (auto it = surfaces.begin(); it != surfaces.end();) {
        const bool inView = it->index.isValid() && view->visualRect(it->index).intersects(viewport);
        if (inView) {
            ++it;
        } else {
            undecided.remove(it.key());
            it = surfaces.erase(it);
        }
    }
}

QSet<GifKey> GifPlayback::chooseWanted(Starts starts) const
{
    struct Candidate
    {
        GifKey key;
        bool hovered;
        bool alreadyWanted;
        int distanceFromCentre;
    };

    const QRect viewport = view->viewport()->rect();
    QList<Candidate> candidates;
    for (auto it = surfaces.constBegin(); it != surfaces.constEnd(); ++it) {
        const GifKey &key = it.key();
        const bool hovered = hoveredKey == key;
        if (unplayable.contains(key) || (mode == GifPlayMode::OnHover && !hovered))
            continue;
        if (starts == Starts::None && !wanted.contains(key))
            continue;

        const QRect rect = viewportRect(*it);
        if (visibleFraction(rect, viewport) < VisibleFractionToPlay)
            continue;

        candidates.append({ key, hovered, wanted.contains(key), (rect.center() - viewport.center()).manhattanLength() });
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        if (a.hovered != b.hovered)
            return a.hovered;
        if (a.alreadyWanted != b.alreadyWanted)
            return a.alreadyWanted;
        return a.distanceFromCentre < b.distanceFromCentre;
    });

    QSet<GifKey> chosen;
    for (const Candidate &candidate : std::as_const(candidates)) {
        if (chosen.size() >= maxPlayingAtOnce)
            break;
        chosen.insert(candidate.key);
    }
    return chosen;
}

void GifPlayback::reconcile(Starts starts)
{
    if (starts == Starts::Allowed) {
        startTimer.stop();
        undecided.clear();
    }

    dropSurfacesOutOfView();
    hoveredKey = keyUnderCursor();

    const bool open = gateOpen();
    const QSet<GifKey> nowWanted = open ? chooseWanted(starts) : QSet<GifKey>();

    const QList<GifKey> opened = openClips.keys();
    for (const GifKey &key : opened) {
        if (nowWanted.contains(key))
            continue;

        const bool resumesWhenGateReopens = !open && surfaces.contains(key);
        if (resumesWhenGateReopens)
            player->setPlaying(openClips[key].id, false);
        else
            closeClip(key);
    }

    const QSet<GifKey> previouslyWanted = std::exchange(wanted, nowWanted);
    for (const GifKey &key : nowWanted)
        play(key);

    for (const GifKey &key : (previouslyWanted - nowWanted) + (nowWanted - previouslyWanted))
        repaint(key);

    const bool holdingPausedClips = !open && !openClips.isEmpty();
    if (!holdingPausedClips)
        pausedTooLongTimer.stop();
    else if (!pausedTooLongTimer.isActive())
        pausedTooLongTimer.start();
}

void GifPlayback::play(const GifKey &key)
{
    const auto surface = surfaces.constFind(key);
    if (surface == surfaces.constEnd())
        return;

    const QSize boundPixels = surface->rectInRow.size() * view->viewport()->devicePixelRatioF();

    auto open = openClips.find(key);
    const bool decodedForAnotherLook = open != openClips.end() &&
                                       (open->url != surface->url || open->boundPixels != boundPixels || open->fit != surface->fit);
    if (decodedForAnotherLook) {
        closeClip(key);
        open = openClips.end();
    }

    if (open == openClips.end()) {
        const QString path = imageManager->rawDownloadPath(surface->url, AsDownloaded);
        if (path.isEmpty()) {
            const auto *chatModel = qobject_cast<const ChatModel *>(view->model());
            if (chatModel)
                imageManager->downloadToCache(surface->url, AsDownloaded, chatModel->getAccountId());
            return;
        }

        const ClipId id = player->open(path, boundPixels, aspectModeFor(surface->fit));
        open = openClips.insert(key, { id, surface->url, boundPixels, surface->fit });
        keyOfClip.insert(id, key);
    }

    player->setPlaying(open->id, true);
}

void GifPlayback::closeClip(const GifKey &key)
{
    const auto open = openClips.constFind(key);
    if (open == openClips.constEnd())
        return;

    player->close(open->id);
    keyOfClip.remove(open->id);
    openClips.erase(open);
    repaint(key);
}

void GifPlayback::closeClipsPausedTooLong()
{
    if (gateOpen())
        return;

    const QList<GifKey> opened = openClips.keys();
    for (const GifKey &key : opened)
        closeClip(key);
}

void GifPlayback::repaint(const GifKey &key)
{
    const auto surface = surfaces.constFind(key);
    if (surface != surfaces.constEnd() && surface->index.isValid())
        view->viewport()->update(viewportRect(*surface));
}

void GifPlayback::repaintAll()
{
    for (auto it = surfaces.constBegin(); it != surfaces.constEnd(); ++it)
        repaint(it.key());
}

void GifPlayback::reconcileAndRepaint()
{
    reconcile(Starts::Allowed);
    repaintAll();
}

void GifPlayback::onDownloadReady(const QUrl &url, const QSize &size)
{
    if (size != AsDownloaded)
        return;

    const bool awaited = std::any_of(wanted.constBegin(), wanted.constEnd(), [this, &url](const GifKey &key) {
        const auto surface = surfaces.constFind(key);
        return surface != surfaces.constEnd() && surface->url == url && !openClips.contains(key);
    });
    if (awaited)
        reconcile(Starts::Allowed);
}

void GifPlayback::onDownloadFailed(const QUrl &url, const QSize &size)
{
    if (size != AsDownloaded)
        return;

    bool gaveUpOnAny = false;
    for (auto it = surfaces.constBegin(); it != surfaces.constEnd(); ++it) {
        if (it->url != url || openClips.contains(it.key()))
            continue;
        unplayable.insert(it.key());
        gaveUpOnAny = true;
    }

    if (gaveUpOnAny)
        reconcile(Starts::Allowed);
}

void GifPlayback::onFrameChanged(quint64 clip)
{
    const auto key = keyOfClip.constFind(clip);
    if (key != keyOfClip.constEnd())
        repaint(*key);
}

void GifPlayback::onClipFailed(quint64 clip)
{
    const auto found = keyOfClip.constFind(clip);
    if (found == keyOfClip.constEnd())
        return;

    const GifKey key = *found;
    unplayable.insert(key);
    wanted.remove(key);
    closeClip(key);
    scheduleReconcile();
}

} // namespace UI
} // namespace Acheron
