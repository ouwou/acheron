#include "UI/Call/CallView.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Streams/StreamBroadcaster.hpp"
#include "Core/Streams/StreamWatcher.hpp"
#include "Core/Video/CallVideo.hpp"
#include "Core/Video/CameraSender.hpp"
#include "UI/Call/CallControlsBar.hpp"
#include "UI/Call/CallStats.hpp"
#include "UI/Call/CallTile.hpp"
#include "UI/Video/VideoSurface.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>

namespace Acheron {
namespace UI {

using Core::CameraSender;
using Core::Snowflake;
using Core::StreamBroadcaster;
using Core::StreamSession;

namespace {

constexpr int MAX_VOLUME_PERCENT = 200;
constexpr int MIN_STAGE_HEIGHT = 60;
constexpr QSize POPPED_OUT_SIZE(960, 600);

const char *const COLLAPSED_WITHOUT_VIDEO_KEY = "video/call_view_collapsed_without_video";
const char *const GEOMETRY_KEY = "video/call_view_geometry";

// The official client mirrors the camera for its owner alone.
QImage mirrored(const QImage &picture)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    return picture.flipped(Qt::Horizontal);
#else
    return picture.mirrored(true, false);
#endif
}

} // namespace

CallView::CallView(Core::Audio::VoiceManager *voice, Core::StreamWatcher *watcher, StreamBroadcaster *broadcaster, CameraSender *camera, Core::ImageManager *images, Snowflake accountId, NameResolver names, AvatarResolver avatars, const QString &title, QWidget *dock)
    : QWidget(dock), voice(voice), watcher(watcher), broadcaster(broadcaster), camera(camera), video(voice->video()), images(images), accountId(accountId), channel(voice->currentChannelId()), title(title), names(std::move(names)), avatars(std::move(avatars)), dock(dock)
{
    setWindowTitle(title);
    collapsed = QSettings().value(COLLAPSED_WITHOUT_VIDEO_KEY, true).toBool();

    controls = new CallControlsBar(broadcaster, camera, QSettings().value(nonVideoSettingsKey(), true).toBool(), this);
    connect(controls, &CallControlsBar::collapseClicked, this, &CallView::toggleCollapsedByHand);
    connect(controls, &CallControlsBar::nonVideoShownChanged, this, &CallView::onNonVideoShownChanged);
    connect(controls, &CallControlsBar::statsShownChanged, this, &CallView::onStatsShownChanged);
    connect(controls, &CallControlsBar::popOutClicked, this, [this] { setPoppedOut(!poppedOut); });
    connect(controls, &CallControlsBar::fullScreenClicked, this, &CallView::toggleFullScreen);

    stage = new QWidget(this);
    paintBlack(stage);
    stage->setMinimumHeight(MIN_STAGE_HEIGHT);
    stage->installEventFilter(this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(controls);
    layout->addWidget(stage, 1);

    if (dock && dock->layout())
        dock->layout()->addWidget(this);

    statsTimer.setInterval(1000);
    connect(&statsTimer, &QTimer::timeout, this, &CallView::refreshStats);

    connect(voice, &Core::Audio::VoiceManager::participantJoined, this, &CallView::rebuild);
    connect(voice, &Core::Audio::VoiceManager::participantLeft, this, &CallView::rebuild);
    connect(voice, &Core::Audio::VoiceManager::participantUpdated, this, &CallView::rebuild);
    connect(voice, &Core::Audio::VoiceManager::participantsCleared, this, &CallView::rebuild);
    connect(voice, &Core::Audio::VoiceManager::voiceStateChanged, this, &CallView::applyChrome);
    connect(voice, &Core::Audio::VoiceManager::participantSpeakingChanged, this, [this](Snowflake userId, bool speaking) {
        if (CallTile *tile = tileAt(TileKey::personOf(userId)))
            tile->surface()->setSpeaking(speaking);
    });
    connect(video, &Core::CallVideo::sendersChanged, this, &CallView::rebuild);
    // Not at once: it may come from a tile's own menu, and takes the tile away.
    connect(video, &Core::CallVideo::videoDisabledChanged, this, &CallView::rebuild, Qt::QueuedConnection);
    connect(watcher, &Core::StreamWatcher::sessionOpened, this, &CallView::onStreamOpened);
    connect(watcher, &Core::StreamWatcher::sessionClosed, this, &CallView::rebuild, Qt::QueuedConnection);
    if (broadcaster) {
        // Not at once: it may come from the own tile's menu, and takes the tile away.
        connect(broadcaster, &StreamBroadcaster::stateChanged, this, &CallView::rebuild, Qt::QueuedConnection);
        connect(broadcaster, &StreamBroadcaster::previewChanged, this, &CallView::showOwnPreview);
        connect(broadcaster, &StreamBroadcaster::failed, this, &CallView::reveal);
    }
    if (camera) {
        // Not at once: it may come from the own tile's menu, and takes the tile away.
        connect(camera, &CameraSender::stateChanged, this, &CallView::rebuild, Qt::QueuedConnection);
        connect(camera, &CameraSender::previewChanged, this, &CallView::showOwnCameraPreview);
        connect(camera, &CameraSender::failed, this, &CallView::reveal);
    }
    if (images)
        connect(images, &Core::ImageManager::imageFetched, this, &CallView::onImageFetched);

    applyChrome();
    rebuild();
}

CallView::~CallView()
{
    // Before the stage takes the surfaces with it, which they would tell of as they go.
    tiles.clear();
    if (video)
        video->setFocused(Snowflake::Invalid);
}

QString CallView::nonVideoSettingsKey() const
{
    return QStringLiteral("video/show_non_video/%1").arg(static_cast<quint64>(channel));
}

QString CallView::nameOf(Snowflake userId) const
{
    return names ? names(userId) : QString::number(static_cast<quint64>(userId));
}

Discord::StreamKey CallView::streamKeyFor(Snowflake userId) const
{
    return Discord::StreamKey::forChannel(voice->currentGuildId(), voice->currentChannelId(), userId);
}

CallTile *CallView::tileAt(const TileKey &key) const
{
    const auto found = tiles.find(key);
    return found != tiles.end() ? found->second.get() : nullptr;
}

void CallView::removeTile(const TileKey &key)
{
    tiles.erase(key);
    if (focused == key)
        focused.reset();
}

void CallView::addTile(const TileKey &key)
{
    auto tile = std::make_unique<CallTile>(key, stage, video);
    VideoSurface *surface = tile->surface();
    connect(surface, &VideoSurface::geometryChanged, this, [this, key](int pixelCount, bool visible) {
        if (CallTile *found = tileAt(key))
            found->announceGeometry(pixelCount, visible);
    });
    connect(surface, &VideoSurface::clicked, this, [this, key] { onTileClicked(key); });
    connect(surface, &VideoSurface::doubleClicked, this, [this, key] { onTileDoubleClicked(key); });
    connect(surface, &QWidget::customContextMenuRequested, this, [this, key, surface](const QPoint &pos) { showTileMenu(key, surface->mapToGlobal(pos)); });
    tiles.emplace(key, std::move(tile));

    if (isOwnShare(key))
        showOwnPreview(broadcaster->preview());
    else if (isOwnCamera(key))
        showOwnCameraPreview(camera->preview());
}

void CallView::rebuild()
{
    if (!voice || !watcher)
        return;

    const QList<Snowflake> cameras = video ? video->senders() : QList<Snowflake>();
    CallFacts facts;
    facts.accountId = accountId;
    facts.ownShareShown = ownShareShown();
    facts.ownCameraShown = ownCameraShown();
    facts.nonVideoShown = controls->nonVideoShown();
    facts.collapsedToBar = !poppedOut && collapsed;
    QStringList participantNames;
    for (const Core::Audio::VoiceParticipant &participant : voice->currentParticipants()) {
        const bool sending = cameras.contains(participant.userId);
        facts.participants.append({ participant.userId, participant.selfStream, sending, sending && video->isVideoDisabled(participant.userId) });
        if (participant.userId != accountId)
            participantNames.append(nameOf(participant.userId));
    }
    const TilePlan plan = planTiles(facts);
    controls->setSummary(callSummary(participantNames, plan.videoCount));

    const bool anyVideo = plan.videoCount > 0;
    const bool videoStarted = anyVideo && !videoPresent;
    videoPresent = anyVideo;
    if (videoStarted && !collapsedByHand)
        collapsed = false;
    applyChrome();

    for (auto it = tiles.begin(); it != tiles.end();) {
        const TileKey key = it->first;
        ++it;
        if (!plan.tiles.contains(key))
            removeTile(key);
    }
    for (const TileKey &key : plan.tiles) {
        if (!tileAt(key))
            addTile(key);
    }
    for (auto &entry : tiles)
        refreshTile(*entry.second);

    if (video) {
        const bool cameraFocused = focused && !focused->isStream();
        video->setFocused(cameraFocused ? focused->userId : Snowflake::Invalid);
    }
    layoutTiles();
    updatePresence();
}

QString CallView::callSummary(const QStringList &participantNames, int videoCount) const
{
    QStringList parts{ title };
    if (!participantNames.isEmpty())
        parts.append(participantNames.join(QStringLiteral(", ")));
    if (videoCount > 0)
        parts.append(tr("%n with video", nullptr, videoCount));
    return parts.join(QStringLiteral(" · "));
}

void CallView::updatePresence()
{
    // With nothing left to show, a window of its own has no reason to stay up.
    if (!videoPresent && poppedOut) {
        setPoppedOut(false);
        return;
    }
    show();
    emit presenceChanged();
}

void CallView::refreshTile(CallTile &tile)
{
    const TileKey key = tile.key();
    VideoSurface *surface = tile.surface();
    const QString name = nameOf(key.userId);
    requestAvatar(tile);

    if (isOwnShare(key)) {
        surface->setCaption(name);
        applyOwnShareState(tile);
        return;
    }
    if (isOwnCamera(key)) {
        surface->setCaption(name);
        applyOwnCameraState(tile);
        return;
    }

    if (key.isStream()) {
        surface->setCaption(name);
        StreamSession *session = watcher->session(streamKeyFor(key.userId).toString());
        if (session != tile.session())
            bindSession(tile, session);
        applySessionState(tile);
        return;
    }

    const Core::Audio::VoiceParticipant *participant = voice->participant(key.userId);
    QString caption = name;
    if (participant && (participant->selfDeaf || participant->serverDeaf))
        caption = tr("%1 (deafened)").arg(name);
    else if (participant && (participant->selfMute || participant->serverMute))
        caption = tr("%1 (muted)").arg(name);
    surface->setCaption(caption);
    surface->setSpeaking(participant && participant->speaking);
    tile.setHasCamera(video && video->senders().contains(key.userId));
    surface->setMessage({});
}

void CallView::bindSession(CallTile &tile, StreamSession *session)
{
    tile.bind(session);
    if (!session)
        return;

    const TileKey key = tile.key();
    // The surface is the context, so this goes when the tile does.
    connect(session, &StreamSession::stateChanged, tile.surface(), [this, key] {
        if (CallTile *found = tileAt(key))
            applySessionState(*found);
        layoutTiles();
    });
}

void CallView::applySessionState(CallTile &tile)
{
    VideoSurface *surface = tile.surface();
    StreamSession *session = tile.session();
    if (!session) {
        surface->setMessage(tr("Watch Stream"));
        return;
    }

    switch (session->state()) {
    case StreamSession::State::Connecting:
        surface->setMessage(tr("Connecting..."));
        break;
    case StreamSession::State::Active:
        surface->setMessage({});
        break;
    case StreamSession::State::Paused:
        surface->setMessage(tr("Stream Paused"), tr("%1 has minimized their application... sit tight!").arg(nameOf(tile.key().userId)));
        break;
    case StreamSession::State::Reconnecting:
        surface->setMessage(tr("You've been disconnected..."), tr("Sit tight, we're patching you back in!"));
        break;
    case StreamSession::State::Ended:
        surface->clear();
        surface->setMessage(tr("This stream has ended."));
        break;
    case StreamSession::State::Failed:
        surface->clear();
        surface->setMessage(tr("Stream failed to start :("));
        break;
    case StreamSession::State::Full:
        surface->clear();
        surface->setMessage(tr("Stream is Full"), tr("Sorry, this stream has the max number of viewers."));
        break;
    }
}

void CallView::onStreamOpened(StreamSession *session)
{
    rebuild();
    // A stream just asked for is what the viewer wants to look at, unless they are looking at
    // someone's camera.
    if (!focused || focused->isStream())
        setFocusedTile(TileKey::streamOf(session->key().ownerId));
}

bool CallView::ownShareShown() const
{
    return broadcaster && (broadcaster->isSharing() || !broadcaster->failure().isEmpty());
}

void CallView::applyOwnShareState(CallTile &tile)
{
    VideoSurface *surface = tile.surface();
    if (!broadcaster)
        return;

    switch (broadcaster->state()) {
    case StreamBroadcaster::State::Idle:
        surface->clear();
        surface->setMessage(tr("Screen share stopped"), broadcaster->failure());
        break;
    case StreamBroadcaster::State::Starting:
    case StreamBroadcaster::State::Connecting:
        surface->setMessage(tr("Starting your screen share..."));
        break;
    case StreamBroadcaster::State::Reconnecting:
        surface->setMessage(tr("You've been disconnected..."), tr("Sit tight, we're patching you back in!"));
        break;
    case StreamBroadcaster::State::Live:
        if (broadcaster->stats().captureStalled)
            surface->setMessage(tr("Stream Paused"), broadcaster->sharesWindow() ? tr("The window is minimized or hidden.") : tr("The screen has stopped delivering pictures."));
        else
            surface->setMessage({});
        break;
    }
}

void CallView::showOwnPreview(const QImage &preview)
{
    CallTile *tile = tileAt(TileKey::streamOf(accountId));
    if (tile && !preview.isNull())
        tile->present(preview);
}

bool CallView::ownCameraShown() const
{
    return camera && camera->state() != CameraSender::State::Off;
}

void CallView::applyOwnCameraState(CallTile &tile)
{
    VideoSurface *surface = tile.surface();
    if (!camera)
        return;

    switch (camera->state()) {
    case CameraSender::State::Off:
        break;
    case CameraSender::State::Starting:
        // The camera's picture is there before the call has been told of it, and stays while
        // the voice connection is made again.
        surface->setMessage(camera->preview().isNull() ? tr("Starting your camera...") : QString());
        break;
    case CameraSender::State::On:
        surface->setMessage({});
        break;
    case CameraSender::State::Failed:
        surface->clear();
        surface->setMessage(tr("Camera stopped"), camera->failure());
        break;
    }
}

void CallView::showOwnCameraPreview(const QImage &preview)
{
    CallTile *tile = tileAt(TileKey::personOf(accountId));
    if (tile && !preview.isNull() && tile->surface()->isVisible())
        tile->present(mirrored(preview));
}

// Watched streams, then cameras, then streams not being watched, then everyone else.
int CallView::rank(const CallTile &tile) const
{
    const TileKey &key = tile.key();
    if (isOwnShare(key))
        return 0;
    if (key.isStream())
        return tile.session() && !tile.session()->isOver() ? 0 : 2;
    return tile.hasCamera() || isOwnCamera(key) ? 1 : 3;
}

void CallView::layoutTiles()
{
    // A tile in its own window has no place here.
    QList<CallTile *> order;
    for (const auto &entry : tiles) {
        if (!entry.second->isPoppedOut())
            order.append(entry.second.get());
    }
    std::sort(order.begin(), order.end(), [this](const CallTile *a, const CallTile *b) {
        if (rank(*a) != rank(*b))
            return rank(*a) < rank(*b);
        const int byName = QString::localeAwareCompare(a->surface()->caption(), b->surface()->caption());
        return byName != 0 ? byName < 0 : a->key() < b->key();
    });

    std::optional<int> focusedIndex;
    for (int i = 0; i < order.size(); i++) {
        if (focused == order[i]->key())
            focusedIndex = i;
    }
    const QList<QRect> places = arrangeTiles(static_cast<int>(order.size()), focusedIndex, stage->rect());
    for (int i = 0; i < order.size(); i++) {
        VideoSurface *surface = order[i]->surface();
        if (places[i].isEmpty()) {
            surface->setVisible(false);
            continue;
        }
        surface->setGeometry(places[i]);
        surface->setVisible(true);
    }
}

void CallView::setFocusedTile(std::optional<TileKey> key)
{
    const CallTile *wanted = key ? tileAt(*key) : nullptr;
    focused = wanted && !wanted->isPoppedOut() ? key : std::nullopt;
    if (video)
        video->setFocused(focused && !focused->isStream() ? focused->userId : Snowflake::Invalid);
    layoutTiles();
}

void CallView::onTileClicked(const TileKey &key)
{
    const CallTile *tile = tileAt(key);
    if (!tile || !watcher)
        return;

    if (isOwnShare(key)) {
        if (broadcaster && !broadcaster->isSharing())
            broadcaster->clearFailure();
        return;
    }
    if (isOwnCamera(key)) {
        if (camera)
            camera->clearFailure();
        return;
    }

    // A stream not being watched is watched.
    if (key.isStream() && (!tile->session() || tile->session()->isOver())) {
        watchClickTile = key;
        watchClickTimer.start();
        watcher->watch(streamKeyFor(key.userId));
    }
}

void CallView::onTileDoubleClicked(const TileKey &key)
{
    CallTile *tile = tileAt(key);
    if (!tile)
        return;
    if (watchClickTile == key && watchClickTimer.isValid() && watchClickTimer.elapsed() < QApplication::doubleClickInterval())
        return;

    if (tile->isPoppedOut()) {
        tile->toggleFullScreen();
        return;
    }
    setFocusedTile(focused == key ? std::nullopt : std::optional<TileKey>(key));
}

void CallView::setTilePoppedOut(const TileKey &key, bool out)
{
    CallTile *tile = tileAt(key);
    if (!tile || out == tile->isPoppedOut())
        return;

    if (out)
        tile->popOut(tile->surface()->caption(), [this, key] { setTilePoppedOut(key, false); });
    else
        tile->dockIn(stage);

    if (focused == key)
        setFocusedTile(std::nullopt);
    else
        layoutTiles();
}

// A menu that is up while the view or the tile goes away goes with it, so it is shown without
// a loop of its own and acts on the tile by its key.
void CallView::showTileMenu(const TileKey &key, const QPoint &globalPos)
{
    const CallTile *tile = tileAt(key);
    if (!tile || !watcher)
        return;

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    StreamSession *session = tile->session();
    const Discord::StreamKey streamKey = streamKeyFor(key.userId);

    if (isOwnShare(key) && broadcaster && broadcaster->isSharing()) {
        connect(menu->addAction(tr("Stop Sharing")), &QAction::triggered, broadcaster.data(), &StreamBroadcaster::stop);
        fillShareQualityMenu(menu->addMenu(tr("Quality")), broadcaster);
    } else if (isOwnCamera(key) && camera && camera->isOn()) {
        connect(menu->addAction(tr("Turn Off Camera")), &QAction::triggered, camera.data(), &CameraSender::turnOff);
    } else if (isOwnShare(key) && broadcaster) {
        connect(menu->addAction(tr("Dismiss")), &QAction::triggered, broadcaster.data(), &StreamBroadcaster::clearFailure);
    } else if (isOwnCamera(key) && camera) {
        connect(menu->addAction(tr("Dismiss")), &QAction::triggered, camera.data(), &CameraSender::clearFailure);
    } else if (key.isStream() && session && !session->isOver()) {
        connect(menu->addAction(tr("Stop Watching")), &QAction::triggered, watcher.data(), [watcher = watcher, streamKey] { watcher->stopWatching(streamKey.toString()); });

        auto *volumeRow = new QWidget(menu);
        auto *rowLayout = new QHBoxLayout(volumeRow);
        rowLayout->setContentsMargins(8, 2, 8, 2);
        rowLayout->addWidget(new QLabel(tr("Stream Volume"), volumeRow));
        auto *slider = new QSlider(Qt::Horizontal, volumeRow);
        slider->setRange(0, MAX_VOLUME_PERCENT);
        slider->setValue(qRound(session->volume() * 100));
        slider->setFixedWidth(120);
        rowLayout->addWidget(slider);
        connect(slider, &QSlider::valueChanged, session, [session](int percent) { session->setVolume(static_cast<float>(percent) / 100.0f); });
        auto *volumeAction = new QWidgetAction(menu);
        volumeAction->setDefaultWidget(volumeRow);
        menu->addAction(volumeAction);
    } else if (key.isStream()) {
        connect(menu->addAction(session ? tr("Close Stream") : tr("Watch Stream")), &QAction::triggered, watcher.data(), [watcher = watcher, streamKey, closing = session != nullptr] {
            if (closing)
                watcher->stopWatching(streamKey.toString());
            else
                watcher->watch(streamKey);
        });
    } else if (tile->hasCamera() && video) {
        connect(menu->addAction(tr("Disable Video")), &QAction::triggered, video.data(), [video = video, userId = key.userId] { video->setVideoDisabled(userId, true); });
    }

    if (!tile->isPoppedOut()) {
        QAction *focus = menu->addAction(tr("Focus"));
        focus->setCheckable(true);
        focus->setChecked(focused == key);
        connect(focus, &QAction::triggered, this, [this, key](bool checked) { setFocusedTile(checked ? std::optional<TileKey>(key) : std::nullopt); });
    }
    QAction *popOutAction = menu->addAction(tr("Pop Out"));
    popOutAction->setCheckable(true);
    popOutAction->setChecked(tile->isPoppedOut());
    connect(popOutAction, &QAction::triggered, this, [this, key](bool checked) { setTilePoppedOut(key, checked); });

    menu->popup(globalPos);
}

void CallView::onStatsShownChanged(bool shown)
{
    if (shown)
        statsTimer.start();
    else
        statsTimer.stop();
    refreshStats();
}

QStringList CallView::statsOf(const CallTile &tile) const
{
    const TileKey &key = tile.key();
    if (isOwnShare(key) && broadcaster && broadcaster->isSharing())
        return CallStats::ofOwnShare(broadcaster->stats());
    if (isOwnCamera(key) && camera && camera->isOn())
        return CallStats::ofOwnCamera(camera->stats());
    if (key.isStream() && tile.session())
        return CallStats::ofStream(tile.session()->stats(), tile.surface());
    if (!key.isStream() && tile.hasCamera() && video)
        return CallStats::ofCamera(video->decoderStats(key.userId), tile.surface());
    return {};
}

void CallView::refreshStats()
{
    const bool shown = controls->statsShown();
    for (const auto &entry : tiles) {
        const CallTile &tile = *entry.second;
        tile.surface()->setStatsText(shown ? statsOf(tile).join(QLatin1Char('\n')) : QString());
    }
}

void CallView::requestAvatar(CallTile &tile)
{
    if (!images || !avatars)
        return;
    const QUrl url = avatars(tile.key().userId);
    if (url.isValid())
        tile.surface()->setPlaceholder(avatarTracker.fetch(images, url, AVATAR_SIZE, tile.key(), accountId));
}

void CallView::onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap)
{
    if (size != AVATAR_SIZE)
        return;
    avatarTracker.notify(url, [this, &pixmap](const TileKey &key) {
        if (CallTile *tile = tileAt(key))
            tile->surface()->setPlaceholder(pixmap);
    });
}

void CallView::popOut(bool fullScreen)
{
    poppedOut = true;
    setParent(nullptr, Qt::Window);
    const QByteArray geometry = QSettings().value(GEOMETRY_KEY).toByteArray();
    if (geometry.isEmpty() || !restoreGeometry(geometry))
        resize(POPPED_OUT_SIZE);
    applyChrome();
    if (fullScreen)
        showFullScreen();
    else
        show();
    raise();
    activateWindow();
    emit presenceChanged();
}

void CallView::setPoppedOut(bool out)
{
    if (out == poppedOut)
        return;
    if (out) {
        popOut(false);
        return;
    }

    if (isFullScreen())
        showNormal();
    else
        QSettings().setValue(GEOMETRY_KEY, saveGeometry());
    poppedOut = false;
    dockAfterFullScreen = false;
    if (!dock || !dock->layout()) {
        // The main window has gone, and with it anywhere to go back to.
        deleteLater();
        return;
    }
    dock->layout()->addWidget(this);
    show();
    rebuild();
}

void CallView::setCollapsed(bool value)
{
    if (collapsed == value)
        return;
    collapsed = value;
    rebuild();
}

void CallView::toggleCollapsedByHand()
{
    collapsedByHand = !collapsed;
    if (!videoPresent)
        QSettings().setValue(COLLAPSED_WITHOUT_VIDEO_KEY, collapsedByHand);
    setCollapsed(collapsedByHand);
}

void CallView::onNonVideoShownChanged(bool shown)
{
    QSettings().setValue(nonVideoSettingsKey(), shown);
    rebuild();
}

void CallView::reveal()
{
    if (!poppedOut) {
        collapsedByHand = false;
        setCollapsed(false);
        return;
    }
    setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
}

void CallView::applyChrome()
{
    const bool barOnly = !poppedOut && collapsed;
    controls->apply({ poppedOut, poppedOut && isFullScreen(), collapsed, videoPresent });
    stage->setVisible(!barOnly);
    setMaximumHeight(barOnly ? controls->sizeHint().height() : QWIDGETSIZE_MAX);
}

void CallView::toggleFullScreen()
{
    // Full screen is a state of a window, so a docked view goes out for it and comes back after.
    if (!poppedOut) {
        popOut(true);
        dockAfterFullScreen = true;
        return;
    }
    if (!isFullScreen()) {
        showFullScreen();
        return;
    }
    if (dockAfterFullScreen)
        setPoppedOut(false);
    else
        showNormal();
}

bool CallView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == stage && event->type() == QEvent::Resize)
        layoutTiles();
    return QWidget::eventFilter(watched, event);
}

void CallView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange)
        applyChrome();
}

void CallView::keyPressEvent(QKeyEvent *event)
{
    // Only in its own window: docked, these keys are the main window's.
    if (poppedOut && togglesFullScreen(event, isFullScreen())) {
        toggleFullScreen();
        return;
    }
    QWidget::keyPressEvent(event);
}

void CallView::closeEvent(QCloseEvent *event)
{
    // Only its own window can be closed, and that puts the view back in the main window.
    event->accept();
    if (poppedOut)
        QTimer::singleShot(0, this, [this] { setPoppedOut(false); });
}

} // namespace UI
} // namespace Acheron
