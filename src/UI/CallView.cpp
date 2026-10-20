#include "CallView.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Streams/StreamBroadcaster.hpp"
#include "Core/Streams/StreamWatcher.hpp"
#include "Core/Theme/Icons.hpp"
#include "Core/Video/CallVideo.hpp"
#include "Core/Video/CameraPipeline.hpp"
#include "Core/Video/CameraSender.hpp"
#include "Core/Video/ScreenCapture.hpp"
#include "UI/ElidedLabel.hpp"
#include "UI/ScreenSharePicker.hpp"
#include "UI/Video/VideoSurface.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
}

namespace Acheron {
namespace UI {

using Core::CameraSender;
using Core::Snowflake;
using Core::StreamBroadcaster;
using Core::StreamSession;
using Core::Video::SendQuality;

namespace {

constexpr int TILE_GAP = 2;
constexpr int MAX_VOLUME_PERCENT = 200;
constexpr int MIN_STAGE_HEIGHT = 60;
constexpr QSize POPPED_OUT_SIZE(960, 600);
constexpr QSize TILE_WINDOW_SIZE(640, 360);

const char *const COLLAPSED_WITHOUT_VIDEO_KEY = "video/call_view_collapsed_without_video";
const char *const GEOMETRY_KEY = "video/call_view_geometry";

const char *const SHARE_ICON = "screen-share";
const char *const STOP_SHARING_ICON = "screen-share-off";
const char *const CAMERA_ICON = "video";
const char *const STOP_CAMERA_ICON = "video-off";

// The camera last chosen, while it is attached. Empty leaves the choice to the pipeline, which
// takes the first there is.
// The official client mirrors the camera for its owner alone.
QImage mirrored(const QImage &picture)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    return picture.flipped(Qt::Horizontal);
#else
    return picture.mirrored(true, false);
#endif
}

// What a screen share and a camera both have to say of what they send.
QStringList sendStatsLines(const Core::Video::PipelineStats &encoder, const Discord::Voice::VideoSendStats &network, int targetBitrate, int receivers)
{
    QStringList lines;
    const QString encoderName = encoder.encoderName.isEmpty() ? CallView::tr("starting") : encoder.encoderName;
    lines << (network.codec.isEmpty() ? CallView::tr("Encoder: %1").arg(encoderName) : CallView::tr("Encoder: %1 by %2").arg(network.codec, encoderName));
    lines << CallView::tr("Resolution: %1x%2").arg(encoder.encodedSize.width()).arg(encoder.encodedSize.height());
    lines << CallView::tr("Frame rate: %1 fps encoded, %2 captured").arg(encoder.encodedFps, 0, 'f', 1).arg(encoder.capturedFps, 0, 'f', 1);
    lines << CallView::tr("Bitrate: %1 kbps sent, %2 kbps asked of the encoder").arg(network.bitrateBps / 1000).arg(targetBitrate / 1000);
    lines << (receivers < 0 ? CallView::tr("Viewers: not reported yet") : CallView::tr("Viewers: %1").arg(receivers));
    lines << (network.lossFraction < 0 ? CallView::tr("Loss: not reported yet") : CallView::tr("Loss: %1%").arg(network.lossFraction * 100.0, 0, 'f', 1));
    if (network.receiverEstimateBps > 0)
        lines << CallView::tr("Receiver's estimate: %1 kbps").arg(network.receiverEstimateBps / 1000);
    if (network.delayEstimateBps > 0)
        lines << CallView::tr("Estimate from delays: %1 kbps, of %2 kbps acknowledged").arg(network.delayEstimateBps / 1000).arg(network.acknowledgedBps / 1000);
    return lines;
}

QString streamTileId(Snowflake userId)
{
    return QStringLiteral("stream:%1").arg(static_cast<quint64>(userId));
}

QString drawingName(const VideoSurface *surface)
{
    if (!surface->drawsWithGpu())
        return CallView::tr("software");
    return surface->drawsFromCard() ? CallView::tr("graphics card, no copy") : CallView::tr("graphics card");
}

QString userTileId(Snowflake userId)
{
    return QStringLiteral("user:%1").arg(static_cast<quint64>(userId));
}

void paintBlack(QWidget *widget)
{
    QPalette black = widget->palette();
    black.setColor(QPalette::Window, Qt::black);
    widget->setPalette(black);
    widget->setAutoFillBackground(true);
}

// A surface draws decoded frames, so a picture that never was one is handed to it as one.
void presentPicture(VideoSurface *surface, const QImage &picture)
{
    const QImage pixels = picture.convertToFormat(QImage::Format_RGB32);
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return;

    frame->format = AV_PIX_FMT_RGB32;
    frame->width = pixels.width();
    frame->height = pixels.height();
    // Where the surface makes YUV of this for its shader, swscale does it its default way,
    // which is this one.
    frame->colorspace = AVCOL_SPC_SMPTE170M;
    frame->color_range = AVCOL_RANGE_MPEG;
    if (av_frame_get_buffer(frame, 0) == 0) {
        const size_t rowBytes = static_cast<size_t>(pixels.width()) * 4;
        for (int row = 0; row < pixels.height(); row++)
            std::memcpy(frame->data[0] + static_cast<qptrdiff>(row) * frame->linesize[0], pixels.constScanLine(row), rowBytes);
        surface->sink()->present(frame);
    }
    av_frame_free(&frame);
}

// One tile in a window of its own. Closing the window puts the tile back.
class TileWindow : public QWidget
{
public:
    TileWindow(VideoSurface *surface, const QString &title, std::function<void()> closed)
        : QWidget(nullptr, Qt::Window), closed(std::move(closed))
    {
        setWindowTitle(title);
        paintBlack(this);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(surface);
        resize(TILE_WINDOW_SIZE);
    }

    void toggleFullScreen()
    {
        if (isFullScreen())
            showNormal();
        else
            showFullScreen();
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        event->accept();
        QTimer::singleShot(0, this, closed);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape && isFullScreen())
            showNormal();
        else if (event->key() == Qt::Key_F11 || event->key() == Qt::Key_F)
            toggleFullScreen();
        else
            QWidget::keyPressEvent(event);
    }

private:
    std::function<void()> closed;
};

} // namespace

CallView::CallView(Core::Audio::VoiceManager *voice, Core::StreamWatcher *watcher, StreamBroadcaster *broadcaster, CameraSender *camera, Core::ImageManager *images, Snowflake accountId, NameResolver names, AvatarResolver avatars, const QString &title, QWidget *dock)
    : QWidget(dock), voice(voice), watcher(watcher), broadcaster(broadcaster), camera(camera), video(voice->video()), images(images), accountId(accountId), names(std::move(names)), avatars(std::move(avatars)), dock(dock)
{
    setWindowTitle(title);
    collapsed = QSettings().value(COLLAPSED_WITHOUT_VIDEO_KEY, true).toBool();

    controls = new QWidget(this);
    auto *bar = new QHBoxLayout(controls);
    bar->setContentsMargins(4, 2, 4, 2);
    bar->setSpacing(8);

    collapseButton = new QToolButton(controls);
    collapseButton->setAutoRaise(true);
    bar->addWidget(collapseButton);
    connect(collapseButton, &QToolButton::clicked, this, &CallView::toggleCollapsedByHand);

    summaryLabel = new ElidedLabel(title, controls);
    bar->addWidget(summaryLabel, 1);

    captureAvailable = broadcaster && (Core::Video::screenCaptureAvailable() || Core::Video::windowCaptureAvailable());
    shareButton = new QToolButton(controls);
    shareButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    bar->addWidget(shareButton);
    connect(shareButton, &QToolButton::clicked, this, &CallView::onShareClicked);
    shareQualityMenu = new QMenu(shareButton);
    connect(shareQualityMenu, &QMenu::aboutToShow, this, [this] { fillQualityMenu(shareQualityMenu); });

    cameraAvailable = camera && Core::Video::cameraCaptureAvailable();
    cameraButton = new QToolButton(controls);
    cameraButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    cameraButton->setPopupMode(QToolButton::MenuButtonPopup);
    bar->addWidget(cameraButton);
    connect(cameraButton, &QToolButton::clicked, this, &CallView::onCameraClicked);
    cameraMenu = new QMenu(cameraButton);
    cameraButton->setMenu(cameraMenu);
    connect(cameraMenu, &QMenu::aboutToShow, this, &CallView::fillCameraMenu);

    nonVideoCheck = new QCheckBox(tr("Show Non-Video Participants"), controls);
    nonVideoCheck->setChecked(QSettings().value(settingsKey(), true).toBool());
    bar->addWidget(nonVideoCheck);
    connect(nonVideoCheck, &QCheckBox::toggled, this, [this](bool shown) {
        QSettings().setValue(settingsKey(), shown);
        rebuild();
    });

    statsButton = new QPushButton(tr("Stats"), controls);
    statsButton->setCheckable(true);
    bar->addWidget(statsButton);
    connect(statsButton, &QPushButton::toggled, this, &CallView::setStatsShown);

    popOutButton = new QPushButton(controls);
    bar->addWidget(popOutButton);
    connect(popOutButton, &QPushButton::clicked, this, [this] { setPoppedOut(!poppedOut); });

    fullScreenButton = new QPushButton(controls);
    bar->addWidget(fullScreenButton);
    connect(fullScreenButton, &QPushButton::clicked, this, &CallView::toggleFullScreen);

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
        auto it = tiles.find(userTileId(userId));
        if (it != tiles.end())
            it->surface->setSpeaking(speaking);
    });
    connect(video, &Core::CallVideo::sendersChanged, this, &CallView::rebuild);
    // Not at once: it may come from a tile's own menu, and takes the tile away.
    connect(video, &Core::CallVideo::videoDisabledChanged, this, &CallView::rebuild, Qt::QueuedConnection);
    connect(watcher, &Core::StreamWatcher::sessionOpened, this, [this](StreamSession *session) {
        rebuild();
        // A stream just asked for is what the viewer wants to look at.
        if (focused.isEmpty() || focused.startsWith(QLatin1String("stream:")))
            setFocusedTile(streamTileId(session->key().ownerId));
    });
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
    for (auto it = tiles.begin(); it != tiles.end(); ++it) {
        if (!it->stream && video)
            video->removeTile(it->userId);
        if (it->stream && it->session)
            it->session->setSurface(0, false);
        // Takes its surface with it. The others go with the stage.
        delete it->popout.data();
    }
    if (video)
        video->setFocused(Snowflake::Invalid);
}

QString CallView::settingsKey() const
{
    return QStringLiteral("video/show_non_video/%1").arg(static_cast<quint64>(voice ? voice->currentChannelId() : Snowflake(0)));
}

Discord::StreamKey CallView::streamKeyFor(Snowflake userId) const
{
    return Discord::StreamKey::forChannel(voice->currentGuildId(), voice->currentChannelId(), userId);
}

QStringList CallView::tileIds() const
{
    return tiles.keys();
}

bool CallView::isTilePoppedOut(const QString &tileId) const
{
    const auto tile = tiles.constFind(tileId);
    return tile != tiles.constEnd() && tile->popout;
}

void CallView::removeTile(const QString &tileId)
{
    const Tile tile = tiles.take(tileId);
    if (!tile.stream && tile.camera && video)
        video->removeTile(tile.userId);
    if (tile.stream && tile.session)
        tile.session->setSurface(0, false);
    delete tile.surface;
    delete tile.popout.data();
    if (focused == tileId)
        focused.clear();
}

void CallView::rebuild()
{
    if (!voice || !watcher)
        return;

    const QList<Core::Audio::VoiceParticipant> participants = voice->currentParticipants();
    const QList<Snowflake> cameras = video ? video->senders() : QList<Snowflake>();

    const auto showsCamera = [this, &cameras](Snowflake userId) { return cameras.contains(userId) && !video->isVideoDisabled(userId); };

    int videoCount = (ownShareShown() ? 1 : 0) + (ownCameraShown() ? 1 : 0);
    QStringList participantNames;
    for (const Core::Audio::VoiceParticipant &participant : participants) {
        if (participant.userId == accountId)
            continue;
        participantNames.append(names ? names(participant.userId) : QString::number(static_cast<quint64>(participant.userId)));
        videoCount += (participant.selfStream ? 1 : 0) + (showsCamera(participant.userId) ? 1 : 0);
    }
    const bool anyVideo = videoCount > 0;
    summaryLabel->setText(callSummary(participantNames, videoCount));

    const bool videoStarted = anyVideo && !videoPresent;
    videoPresent = anyVideo;
    if (videoStarted && !collapsedByHand)
        collapsed = false;
    applyChrome();

    QHash<QString, QPair<Snowflake, bool>> wanted;
    // The bar alone, over a call without video, has no use for tiles to draw on.
    const bool tilesShown = anyVideo || poppedOut || !collapsed;
    if (ownShareShown() && tilesShown)
        wanted.insert(streamTileId(accountId), { accountId, true });
    if (ownCameraShown() && tilesShown)
        wanted.insert(userTileId(accountId), { accountId, false });
    for (const Core::Audio::VoiceParticipant &participant : participants) {
        if (participant.userId == accountId || !tilesShown)
            continue;
        if (participant.selfStream)
            wanted.insert(streamTileId(participant.userId), { participant.userId, true });
        // A camera whose video has been disabled has no tile at all. It is turned back on
        // from the person's entry in the channel list.
        const bool disabled = cameras.contains(participant.userId) && !showsCamera(participant.userId);
        if (!disabled && (showsCamera(participant.userId) || nonVideoCheck->isChecked() || !anyVideo))
            wanted.insert(userTileId(participant.userId), { participant.userId, false });
    }

    const QStringList existing = tiles.keys();
    for (const QString &id : existing) {
        if (!wanted.contains(id))
            removeTile(id);
    }

    for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it) {
        if (tiles.contains(it.key()))
            continue;

        const QString id = it.key();
        Tile tile;
        tile.userId = it.value().first;
        tile.stream = it.value().second;
        tile.surface = new VideoSurface(stage);
        tile.surface->setMinimumSize(1, 1);
        tile.surface->setContextMenuPolicy(Qt::CustomContextMenu);
        tile.surface->show();

        connect(tile.surface, &VideoSurface::geometryChanged, this, [this, id](int pixelCount, bool visible) {
            auto found = tiles.find(id);
            if (found == tiles.end())
                return;
            if (found->stream && found->session)
                found->session->setSurface(pixelCount, visible);
            else if (!found->stream && found->camera && video)
                video->setTile(found->userId, found->surface->sink(), pixelCount, visible);
        });
        connect(tile.surface, &VideoSurface::clicked, this, [this, id] { onTileClicked(id); });
        connect(tile.surface, &VideoSurface::doubleClicked, this, [this, id] { onTileDoubleClicked(id); });
        connect(tile.surface, &QWidget::customContextMenuRequested, this, [this, id](const QPoint &pos) {
            auto found = tiles.constFind(id);
            if (found != tiles.constEnd())
                showTileMenu(id, found->surface->mapToGlobal(pos));
        });
        tiles.insert(id, tile);
        if (isOwnShare(tile))
            showOwnPreview(broadcaster->preview());
        else if (isOwnCamera(tile))
            showOwnCameraPreview(camera->preview());
    }

    for (auto it = tiles.begin(); it != tiles.end(); ++it)
        refreshTile(it.value());

    if (video) {
        const auto focusedTile = tiles.constFind(focused);
        video->setFocused(focusedTile != tiles.constEnd() && !focusedTile->stream ? focusedTile->userId : Snowflake::Invalid);
    }
    layoutTiles();
    updatePresence();
}

QString CallView::callSummary(const QStringList &participantNames, int videoCount) const
{
    QStringList parts{ windowTitle() };
    if (!participantNames.isEmpty())
        parts.append(participantNames.join(QStringLiteral(", ")));
    if (videoCount > 0)
        parts.append(tr("%n with video", nullptr, videoCount));
    return parts.join(QStringLiteral(" \u00b7 "));
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

void CallView::refreshTile(Tile &tile)
{
    const QString name = names ? names(tile.userId) : QString::number(static_cast<quint64>(tile.userId));
    requestAvatar(tile);

    if (isOwnShare(tile)) {
        tile.surface->setCaption(name);
        applyOwnShareState(tile);
        return;
    }
    if (isOwnCamera(tile)) {
        tile.surface->setCaption(name);
        applyOwnCameraState(tile);
        return;
    }

    if (tile.stream) {
        tile.surface->setCaption(name);
        StreamSession *session = watcher->session(streamKeyFor(tile.userId).toString());
        if (session != tile.session)
            bindSession(tile, session);
        applySessionState(tile);
        return;
    }

    const Core::Audio::VoiceParticipant *participant = voice->participant(tile.userId);
    QString caption = name;
    if (participant && (participant->selfDeaf || participant->serverDeaf))
        caption = tr("%1 (deafened)").arg(name);
    else if (participant && (participant->selfMute || participant->serverMute))
        caption = tr("%1 (muted)").arg(name);
    tile.surface->setCaption(caption);
    tile.surface->setSpeaking(participant && participant->speaking);

    const bool camera = video && video->senders().contains(tile.userId);
    if (camera != tile.camera) {
        tile.camera = camera;
        if (camera) {
            tile.surface->announceGeometry();
        } else {
            if (video)
                video->removeTile(tile.userId);
            tile.surface->clear();
        }
    }

    tile.surface->setMessage({});
}

void CallView::bindSession(Tile &tile, StreamSession *session)
{
    tile.session = session;
    tile.surface->clear();
    if (!session)
        return;

    const QString id = streamTileId(tile.userId);
    // The surface is the context, so this goes when the tile does.
    connect(session, &StreamSession::stateChanged, tile.surface, [this, id] {
        auto found = tiles.find(id);
        if (found != tiles.end())
            applySessionState(found.value());
        layoutTiles();
    });

    session->setSink(tile.surface->sink());
    tile.surface->announceGeometry();
}

void CallView::applySessionState(Tile &tile)
{
    VideoSurface *surface = tile.surface;
    if (!tile.session) {
        surface->setMessage(tr("Watch Stream"));
        return;
    }

    const QString name = names ? names(tile.userId) : QString::number(static_cast<quint64>(tile.userId));
    switch (tile.session->state()) {
    case StreamSession::State::Connecting:
        surface->setMessage(tr("Connecting..."));
        break;
    case StreamSession::State::Active:
        surface->setMessage({});
        break;
    case StreamSession::State::Paused:
        surface->setMessage(tr("Stream Paused"), tr("%1 has minimized their application... sit tight!").arg(name));
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

bool CallView::ownShareShown() const
{
    return broadcaster && (broadcaster->isSharing() || !broadcaster->failure().isEmpty());
}

void CallView::applyOwnShareState(Tile &tile)
{
    VideoSurface *surface = tile.surface;
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
            surface->setMessage(tr("Stream Paused"), sharingWindow ? tr("The window is minimized or hidden.") : tr("The screen has stopped delivering pictures."));
        else
            surface->setMessage({});
        break;
    }
}

void CallView::showOwnPreview(const QImage &preview)
{
    const auto tile = tiles.constFind(streamTileId(accountId));
    if (tile != tiles.constEnd() && !preview.isNull())
        presentPicture(tile->surface, preview);
}

bool CallView::ownCameraShown() const
{
    return camera && camera->state() != CameraSender::State::Off;
}

void CallView::applyOwnCameraState(Tile &tile)
{
    VideoSurface *surface = tile.surface;
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
    const auto tile = tiles.constFind(userTileId(accountId));
    if (tile != tiles.constEnd() && !preview.isNull() && tile->surface->isVisible())
        presentPicture(tile->surface, mirrored(preview));
}

void CallView::onCameraClicked()
{
    if (!camera)
        return;
    if (camera->isOn())
        camera->turnOff();
    else
        camera->turnOn(Core::Video::preferredCamera(Core::Video::cameraDevices()));
}

void CallView::fillCameraMenu()
{
    cameraMenu->clear();

    const QList<Core::Video::CameraDevice> devices = Core::Video::cameraDevices();
    if (devices.isEmpty()) {
        cameraMenu->addAction(tr("No camera found"))->setEnabled(false);
        return;
    }

    const QString remembered = Core::Video::preferredCamera(devices);
    auto *choices = new QActionGroup(cameraMenu);
    for (const Core::Video::CameraDevice &device : devices) {
        QAction *action = cameraMenu->addAction(device.name);
        action->setCheckable(true);
        action->setChecked(remembered.isEmpty() ? device.id == devices.first().id : device.id == remembered);
        choices->addAction(action);
        connect(action, &QAction::triggered, this, [this, id = device.id] { chooseCamera(id); });
    }
}

void CallView::chooseCamera(const QString &deviceId)
{
    Core::Video::setPreferredCamera(deviceId);
    if (camera && camera->isOn())
        camera->turnOn(deviceId);
}

QStringList CallView::ownCameraStats() const
{
    const CameraSender::Stats stats = camera->stats();

    QStringList lines = sendStatsLines(stats.encoder, stats.network, stats.targetBitrate, stats.receivers);
    lines << tr("Wanted by viewers: %1%").arg(stats.servedWant);
    if (stats.mutedFramerate)
        lines << tr("Muted frame rate");
    if (stats.encodingSuspended)
        lines << tr("Encoding suspended: nobody is watching");
    if (stats.hasSecondLayer && stats.secondEncodingSuspended)
        lines << tr("Second layer: not encoded, nobody is watching it");
    else if (stats.hasSecondLayer)
        lines << tr("Second layer: %1x%2, %3 kbps asked of the encoder, wanted %4%, viewers %5").arg(stats.secondEncoder.encodedSize.width()).arg(stats.secondEncoder.encodedSize.height()).arg(stats.secondTargetBitrate / 1000).arg(stats.secondServedWant).arg(stats.secondReceivers < 0 ? tr("not reported yet") : QString::number(stats.secondReceivers));
    return lines;
}

void CallView::onShareClicked()
{
    if (!broadcaster)
        return;
    if (broadcaster->isSharing()) {
        broadcaster->stop();
        return;
    }

    auto *picker = new ScreenSharePicker(broadcaster->tier(), this);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    connect(picker, &ScreenSharePicker::shareRequested, this, [this](const Core::Video::CaptureSource &source, const SendQuality &quality, bool withAudio) {
        if (!broadcaster) {
            Core::Video::releaseSource(source);
            return;
        }
        sharingWindow = source.kind == Core::Video::CaptureSource::Kind::Window;
        Core::Video::restoreIfMinimised(source);
        broadcaster->start(source, quality, withAudio);
    });
    picker->open();
}

void CallView::fillQualityMenu(QMenu *menu)
{
    menu->clear();
    if (!broadcaster)
        return;

    const Core::Video::SharerTier tier = broadcaster->tier();
    const SendQuality current = broadcaster->quality();

    auto *heights = new QActionGroup(menu);
    for (const int height : Core::Video::SEND_HEIGHTS) {
        const SendQuality there = Core::Video::atHeight(current, height, tier);
        QAction *action = menu->addAction(ScreenSharePicker::heightLabel(height));
        action->setCheckable(true);
        action->setChecked(height == current.height);
        action->setEnabled(Core::Video::mayUseQuality(there, tier));
        heights->addAction(action);
        connect(action, &QAction::triggered, this, [this, there] { changeShareQuality(there); });
    }

    menu->addSeparator();
    auto *framerates = new QActionGroup(menu);
    for (const int framerate : Core::Video::SEND_FRAMERATES) {
        const SendQuality atRate{ current.height, framerate };
        QAction *action = menu->addAction(ScreenSharePicker::framerateLabel(framerate));
        action->setCheckable(true);
        action->setChecked(framerate == current.framerate);
        action->setEnabled(Core::Video::mayUseQuality(atRate, tier));
        framerates->addAction(action);
        connect(action, &QAction::triggered, this, [this, atRate] { changeShareQuality(atRate); });
    }
}

void CallView::changeShareQuality(const SendQuality &quality)
{
    if (!broadcaster)
        return;
    broadcaster->setQuality(quality);
    ScreenSharePicker::rememberQuality(broadcaster->quality());
}

QStringList CallView::ownShareStats() const
{
    const StreamBroadcaster::Stats stats = broadcaster->stats();

    QStringList lines = sendStatsLines(stats.encoder, stats.network, stats.targetBitrate, stats.receivers);
    if (stats.audio == StreamBroadcaster::AudioState::Capturing)
        lines << tr("Sound: shared");
    else if (stats.audio == StreamBroadcaster::AudioState::Failed)
        lines << tr("Sound: could not be captured, sharing without it");
    if (stats.encodingSuspended)
        lines << tr("Encoding suspended: nobody is watching");
    if (stats.captureStalled)
        lines << tr("Capture stalled");
    return lines;
}

int CallView::rank(const Tile &tile) const
{
    if (isOwnShare(tile))
        return 0;
    if (tile.stream)
        return tile.session && !tile.session->isOver() ? 0 : 2;
    return tile.camera || isOwnCamera(tile) ? 1 : 3;
}

void CallView::layoutTiles()
{
    // Watched streams, then cameras, then streams not being watched, then everyone else;
    // by name within each. A tile in its own window has no place here.
    QStringList order;
    for (auto it = tiles.constBegin(); it != tiles.constEnd(); ++it) {
        if (!it->popout)
            order.append(it.key());
    }
    if (order.isEmpty())
        return;
    std::sort(order.begin(), order.end(), [this](const QString &a, const QString &b) {
        const Tile &left = tiles[a];
        const Tile &right = tiles[b];
        if (rank(left) != rank(right))
            return rank(left) < rank(right);
        const int byName = QString::localeAwareCompare(left.surface->caption(), right.surface->caption());
        return byName != 0 ? byName < 0 : a < b;
    });

    const QRect area = stage->rect();
    const auto place = [](VideoSurface *surface, const QRect &where) {
        surface->setGeometry(where);
        surface->setVisible(true);
    };

    if (order.contains(focused)) {
        order.removeAll(focused);
        const int stripCount = qMin(static_cast<int>(order.size()), MAX_STRIP_TILES);
        const int stripHeight = stripCount == 0 ? 0 : qBound(54, area.height() / 5, 144);
        const int largeHeight = area.height() - (stripCount == 0 ? 0 : stripHeight + TILE_GAP);
        place(tiles[focused].surface, QRect(area.left(), area.top(), area.width(), largeHeight));

        if (stripCount > 0) {
            const int widest = (area.width() - TILE_GAP * (stripCount - 1)) / stripCount;
            const int tileWidth = qMin(stripHeight * 16 / 9, widest);
            const int used = tileWidth * stripCount + TILE_GAP * (stripCount - 1);
            int x = area.left() + (area.width() - used) / 2;
            for (int i = 0; i < stripCount; i++) {
                place(tiles[order[i]].surface, QRect(x, area.top() + largeHeight + TILE_GAP, tileWidth, stripHeight));
                x += tileWidth + TILE_GAP;
            }
        }
        for (int i = stripCount; i < order.size(); i++)
            tiles[order[i]].surface->setVisible(false);
        return;
    }

    const int count = static_cast<int>(order.size());
    const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const int rows = (count + columns - 1) / columns;
    const int cellWidth = (area.width() - TILE_GAP * (columns - 1)) / columns;
    const int cellHeight = (area.height() - TILE_GAP * (rows - 1)) / rows;
    for (int i = 0; i < count; i++) {
        const int column = i % columns;
        const int row = i / columns;
        place(tiles[order[i]].surface, QRect(area.left() + column * (cellWidth + TILE_GAP), area.top() + row * (cellHeight + TILE_GAP), cellWidth, cellHeight));
    }
}

void CallView::setFocusedTile(const QString &tileId)
{
    const auto wanted = tiles.constFind(tileId);
    focused = wanted != tiles.constEnd() && !wanted->popout ? tileId : QString();
    if (video) {
        const auto tile = tiles.constFind(focused);
        video->setFocused(tile != tiles.constEnd() && !tile->stream ? tile->userId : Snowflake::Invalid);
    }
    layoutTiles();
}

void CallView::onTileClicked(const QString &tileId)
{
    const auto tile = tiles.constFind(tileId);
    if (tile == tiles.constEnd() || !watcher)
        return;

    if (isOwnShare(*tile)) {
        if (broadcaster && !broadcaster->isSharing())
            broadcaster->clearFailure();
        return;
    }
    if (isOwnCamera(*tile)) {
        if (camera)
            camera->clearFailure();
        return;
    }

    // A stream not being watched is watched.
    if (tile->stream && (!tile->session || tile->session->isOver())) {
        watchClickTile = tileId;
        watchClickTimer.start();
        watcher->watch(streamKeyFor(tile->userId));
    }
}

void CallView::onTileDoubleClicked(const QString &tileId)
{
    const auto tile = tiles.constFind(tileId);
    if (tile == tiles.constEnd())
        return;
    if (tileId == watchClickTile && watchClickTimer.isValid() && watchClickTimer.elapsed() < QApplication::doubleClickInterval())
        return;

    if (tile->popout) {
        static_cast<TileWindow *>(tile->popout.data())->toggleFullScreen();
        return;
    }
    setFocusedTile(focused == tileId ? QString() : tileId);
}

void CallView::setTilePoppedOut(const QString &tileId, bool out)
{
    auto tile = tiles.find(tileId);
    if (tile == tiles.end() || out == !tile->popout.isNull())
        return;

    if (out) {
        auto *window = new TileWindow(tile->surface, tile->surface->caption(), [this, tileId] { setTilePoppedOut(tileId, false); });
        tile->popout = window;
        tile->surface->show();
        window->show();
    } else {
        QWidget *window = tile->popout;
        tile->popout = nullptr;
        tile->surface->setParent(stage);
        window->hide();
        window->deleteLater();
    }

    if (focused == tileId)
        setFocusedTile({});
    else
        layoutTiles();
}

void CallView::showTileMenu(const QString &tileId, const QPoint &globalPos)
{
    auto found = tiles.constFind(tileId);
    if (found == tiles.constEnd() || !watcher)
        return;
    const Tile tile = found.value();

    QMenu menu(this);
    QAction *watch = nullptr;
    QAction *stop = nullptr;
    QAction *disable = nullptr;
    QAction *focus = nullptr;
    QAction *stopSharing = nullptr;
    QAction *stopCamera = nullptr;
    QAction *dismiss = nullptr;

    if (isOwnShare(tile) && broadcaster && broadcaster->isSharing()) {
        stopSharing = menu.addAction(tr("Stop Sharing"));
        fillQualityMenu(menu.addMenu(tr("Quality")));
    } else if (isOwnCamera(tile) && camera && camera->isOn()) {
        stopCamera = menu.addAction(tr("Turn Off Camera"));
    } else if (isOwnShare(tile) || isOwnCamera(tile)) {
        dismiss = menu.addAction(tr("Dismiss"));
    } else if (tile.stream && tile.session && !tile.session->isOver()) {
        stop = menu.addAction(tr("Stop Watching"));

        auto *volumeRow = new QWidget(&menu);
        auto *rowLayout = new QHBoxLayout(volumeRow);
        rowLayout->setContentsMargins(8, 2, 8, 2);
        rowLayout->addWidget(new QLabel(tr("Stream Volume"), volumeRow));
        auto *slider = new QSlider(Qt::Horizontal, volumeRow);
        slider->setRange(0, MAX_VOLUME_PERCENT);
        slider->setValue(qRound(tile.session->volume() * 100));
        slider->setFixedWidth(120);
        rowLayout->addWidget(slider);
        connect(slider, &QSlider::valueChanged, tile.session.data(), [session = tile.session](int percent) {
            if (session)
                session->setVolume(static_cast<float>(percent) / 100.0f);
        });
        auto *volumeAction = new QWidgetAction(&menu);
        volumeAction->setDefaultWidget(volumeRow);
        menu.addAction(volumeAction);
    } else if (tile.stream) {
        watch = menu.addAction(tile.session ? tr("Close Stream") : tr("Watch Stream"));
    } else if (tile.camera && video) {
        disable = menu.addAction(tr("Disable Video"));
    }

    if (!tile.popout) {
        focus = menu.addAction(tr("Focus"));
        focus->setCheckable(true);
        focus->setChecked(focused == tileId);
    }
    QAction *popOut = menu.addAction(tr("Pop Out"));
    popOut->setCheckable(true);
    popOut->setChecked(!tile.popout.isNull());

    QAction *chosen = menu.exec(globalPos);
    if (!chosen || !watcher)
        return;

    if (chosen == stopSharing && broadcaster) {
        broadcaster->stop();
    } else if (chosen == stopCamera && camera) {
        camera->turnOff();
    } else if (chosen == dismiss && isOwnCamera(tile) && camera) {
        camera->clearFailure();
    } else if (chosen == dismiss && isOwnShare(tile) && broadcaster) {
        broadcaster->clearFailure();
    } else if (chosen == stop) {
        watcher->stopWatching(streamKeyFor(tile.userId).toString());
    } else if (chosen == watch) {
        if (tile.session)
            watcher->stopWatching(streamKeyFor(tile.userId).toString());
        else
            watcher->watch(streamKeyFor(tile.userId));
    } else if (chosen == disable && video) {
        video->setVideoDisabled(tile.userId, true);
    } else if (chosen == focus) {
        setFocusedTile(focus->isChecked() ? tileId : QString());
    } else if (chosen == popOut) {
        setTilePoppedOut(tileId, popOut->isChecked());
    }
}

void CallView::setStatsShown(bool shown)
{
    if (statsButton->isChecked() != shown) {
        statsButton->setChecked(shown);
        return;
    }
    if (shown)
        statsTimer.start();
    else
        statsTimer.stop();
    refreshStats();
}

void CallView::refreshStats()
{
    const bool shown = statsButton->isChecked();
    for (auto it = tiles.begin(); it != tiles.end(); ++it) {
        if (!shown) {
            it->surface->setStatsText({});
            continue;
        }

        QStringList lines;
        if (isOwnShare(it.value()) && broadcaster && broadcaster->isSharing()) {
            lines = ownShareStats();
        } else if (isOwnCamera(it.value()) && camera && camera->isOn()) {
            lines = ownCameraStats();
        } else if (it->stream && it->session) {
            const StreamSession::Stats stats = it->session->stats();
            lines << tr("Codec: %1 (%2)").arg(stats.network.codec, stats.decoder.decoderName);
            lines << tr("Resolution: %1x%2").arg(stats.decoder.width).arg(stats.decoder.height);
            lines << tr("Frame rate: %1 fps").arg(stats.framesPerSecond, 0, 'f', 1);
            lines << tr("Bitrate: %1 kbps").arg(stats.bitrateKbps, 0, 'f', 0);
            lines << tr("Decode time: %1 ms").arg(stats.decoder.averageDecodeMs, 0, 'f', 2);
            if (stats.pixelShareAskedFor < 1)
                lines << tr("Decoder behind: asking for %1% of the picture").arg(qRound(stats.pixelShareAskedFor * 100));
            lines << tr("Loss: %1%").arg(stats.network.lossFraction * 100.0, 0, 'f', 1);
            if (stats.network.serverEstimateBps != 0)
                lines << tr("Server estimate: %1 kbps, %2% loss").arg(stats.network.serverEstimateBps / 1000).arg(stats.network.serverLossFraction * 100.0, 0, 'f', 1);
            lines << tr("NACK: %1, PLI: %2").arg(stats.network.nacked).arg(stats.network.keyframeRequests);
            lines << tr("Frames: %1 decoded, %2 dropped, %3 errors").arg(stats.decoder.decoded).arg(stats.decoder.dropped).arg(stats.decoder.errors);
            if (stats.network.decryptFailures != 0)
                lines << tr("Decrypt failures: %1").arg(stats.network.decryptFailures);
            lines << tr("Drawing: %1").arg(drawingName(it->surface));
        } else if (!it->stream && it->camera && video) {
            const Core::Media::VideoDecoder::Stats stats = video->decoderStats(it->userId);
            if (!stats.decoderName.isEmpty()) {
                lines << tr("Decoder: %1").arg(stats.decoderName);
                lines << tr("Resolution: %1x%2").arg(stats.width).arg(stats.height);
                lines << tr("Decode time: %1 ms").arg(stats.averageDecodeMs, 0, 'f', 2);
                lines << tr("Frames: %1 decoded, %2 dropped, %3 errors").arg(stats.decoded).arg(stats.dropped).arg(stats.errors);
                lines << tr("Drawing: %1").arg(drawingName(it->surface));
            }
        }
        it->surface->setStatsText(lines.join(QLatin1Char('\n')));
    }
}

void CallView::requestAvatar(Tile &tile)
{
    if (!images || !avatars)
        return;
    const QUrl url = avatars(tile.userId);
    if (!url.isValid())
        return;
    const QString id = tile.stream ? streamTileId(tile.userId) : userTileId(tile.userId);
    tile.surface->setPlaceholder(avatarTracker.fetch(images, url, AVATAR_SIZE, id, accountId));
}

void CallView::onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap)
{
    if (size != AVATAR_SIZE)
        return;
    avatarTracker.notify(url, [this, &pixmap](const QString &id) {
        auto tile = tiles.constFind(id);
        if (tile != tiles.constEnd())
            tile->surface->setPlaceholder(pixmap);
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

// What is shown of the view's own chrome depends on where it is: docked it can be collapsed
// to its bar, and full screen it has no bar at all.
void CallView::applyChrome()
{
    const bool fullScreen = poppedOut && isFullScreen();
    const bool barOnly = !poppedOut && collapsed;
    controls->setVisible(!fullScreen);
    collapseButton->setVisible(!poppedOut);

    const bool sharing = broadcaster && broadcaster->isSharing();
    shareButton->setVisible(captureAvailable && broadcaster);
    shareButton->setEnabled(sharing || (broadcaster && broadcaster->canShare()));
    shareButton->setText(sharing ? tr("Stop Sharing") : tr("Share Screen"));
    shareButton->setIcon(Core::Theme::Icons::icon(QLatin1String(sharing ? STOP_SHARING_ICON : SHARE_ICON), Core::Theme::Token::ButtonText));
    // While sharing, the arrow beside the button changes the quality without stopping.
    shareButton->setPopupMode(sharing ? QToolButton::MenuButtonPopup : QToolButton::DelayedPopup);
    shareButton->setMenu(sharing ? shareQualityMenu : nullptr);

    const bool cameraOn = camera && camera->isOn();
    cameraButton->setVisible(cameraAvailable && camera);
    cameraButton->setEnabled(cameraOn || (camera && camera->canTurnOn()));
    cameraButton->setText(cameraOn ? tr("Stop Camera") : tr("Camera"));
    cameraButton->setToolTip(cameraOn ? tr("Turn Off Camera") : tr("Turn On Camera"));
    cameraButton->setIcon(Core::Theme::Icons::icon(QLatin1String(cameraOn ? STOP_CAMERA_ICON : CAMERA_ICON), Core::Theme::Token::ButtonText));

    nonVideoCheck->setVisible(videoPresent && !barOnly);
    statsButton->setVisible(videoPresent && !barOnly);
    popOutButton->setVisible(videoPresent || poppedOut);
    fullScreenButton->setVisible(videoPresent || poppedOut);
    collapseButton->setArrowType(collapsed ? Qt::RightArrow : Qt::DownArrow);
    popOutButton->setText(poppedOut ? tr("Dock") : tr("Pop Out"));
    fullScreenButton->setText(fullScreen ? tr("Exit Full Screen") : tr("Full Screen"));
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
    if (poppedOut && event->key() == Qt::Key_Escape && isFullScreen()) {
        toggleFullScreen();
        return;
    }
    if (poppedOut && (event->key() == Qt::Key_F11 || event->key() == Qt::Key_F)) {
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
