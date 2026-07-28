#include "UI/VideoFullscreenWindow.hpp"

#include "Core/Video/Player.hpp"

#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>

namespace Acheron {
namespace UI {

namespace {
constexpr int IdleHideMs = 2500;
constexpr qint64 ArrowSeekMs = 5000;
constexpr float VolumeStep = 0.05f;
} // namespace

VideoFullscreenWindow::VideoFullscreenWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    QPalette black = palette();
    black.setColor(QPalette::Window, Qt::black);
    setPalette(black);
    setAutoFillBackground(true);

    idleTimer = new QTimer(this);
    idleTimer->setSingleShot(true);
    idleTimer->setInterval(IdleHideMs);
    connect(idleTimer, &QTimer::timeout, this, [this] {
        if (drag.active())
            return;
        controlsVisible = false;
        volumeExpanded = false;
        setCursor(Qt::BlankCursor);
        update();
    });
}

VideoFullscreenWindow::~VideoFullscreenWindow()
{
    detach();
}

void VideoFullscreenWindow::showPlayer(Core::Video::Player *newPlayer, const QString &newKey,
                                       const QSize &newInlineSize)
{
    if (!newPlayer)
        return;

    detach();

    player = newPlayer;
    key = newKey;
    inlineSize = newInlineSize;
    volumeExpanded = false;
    drag.end();

    connect(player, &Core::Video::Player::frameReady, this, QOverload<>::of(&QWidget::update));
    connect(player, &Core::Video::Player::positionChanged, this, [this](qint64) { update(); });
    connect(player, &Core::Video::Player::stateChanged, this, [this](Core::Video::Player::State) { update(); });

    if (QScreen *screen = QGuiApplication::screenAt(QCursor::pos()))
        setGeometry(screen->geometry());

    showFullScreen();
    raise();
    activateWindow();
    setFocus();

    player->setTargetSize(decodeSize());
    revealControls();
}

void VideoFullscreenWindow::detach()
{
    if (!player)
        return;

    disconnect(player, nullptr, this, nullptr);

    if (inlineSize.isValid() && !inlineSize.isEmpty())
        player->setTargetSize(inlineSize);

    player = nullptr;
    key.clear();
}

VideoControls::State VideoFullscreenWindow::controlState() const
{
    return VideoControls::stateFor(player, volumeExpanded, true);
}

void VideoFullscreenWindow::revealControls()
{
    idleTimer->start();

    if (controlsVisible)
        return;

    controlsVisible = true;
    unsetCursor();
    update();
}

void VideoFullscreenWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);

    const QImage frame = player ? player->currentFrame() : QImage();
    if (frame.isNull()) {
        painter.fillRect(rect(), Qt::black);
    } else {
        const QRect target = VideoControls::fitRect(frame.size(), rect());
        if (target.top() > 0)
            painter.fillRect(QRect(0,
                                   0,
                                   width(),
                                   target.top()),
                             Qt::black);
        if (target.bottom() + 1 < height())
            painter.fillRect(QRect(0,
                                   target.bottom() + 1,
                                   width(),
                                   height() - target.bottom() - 1),
                             Qt::black);
        if (target.left() > 0)
            painter.fillRect(QRect(0,
                                   target.top(),
                                   target.left(),
                                   target.height()),
                             Qt::black);
        if (target.right() + 1 < width())
            painter.fillRect(QRect(target.right() + 1,
                                   target.top(),
                                   width() - target.right() - 1,
                                   target.height()),
                             Qt::black);

        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(target, frame);
    }

    if (!player)
        return;

    const VideoControls::State state = controlState();

    if (!state.playing && player->state() != Core::Video::Player::State::Opening)
        VideoControls::paintPlayBadge(&painter, rect());

    if (controlsVisible)
        VideoControls::paint(&painter, VideoControls::calculate(rect(), state), state);
}

void VideoFullscreenWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !player) {
        QWidget::mousePressEvent(event);
        return;
    }

    revealControls();

    const VideoControls::State state = controlState();
    const VideoControls::Layout layout = VideoControls::calculate(rect(), state);

    if (VideoControls::beginDrag(player, layout, state, event->pos(), drag))
        update();
}

void VideoFullscreenWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!player) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    if (drag.active()) {
        const auto state = controlState();
        const auto layout = VideoControls::calculate(rect(), state);

        VideoControls::applyDrag(player, layout, event->pos(), drag);
        update();
        return;
    }

    const auto state = controlState();
    const auto layout = VideoControls::calculate(rect(), state);
    const QRect zone = VideoControls::volumeHoverZone(layout);
    const bool inZone = !zone.isNull() && zone.contains(event->pos());
    if (inZone != volumeExpanded) {
        volumeExpanded = inZone;
        update();
    }

    revealControls();
}

void VideoFullscreenWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !player) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (drag.active()) {
        drag.end();
        idleTimer->start();
        return;
    }

    const auto state = controlState();
    const auto layout = VideoControls::calculate(rect(), state);

    if (VideoControls::handleRelease(player, layout, state, event->pos()) == VideoControls::ReleaseResult::ToggleFullscreen) {
        close();
        return;
    }

    update();
}

void VideoFullscreenWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        close();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void VideoFullscreenWindow::keyPressEvent(QKeyEvent *event)
{
    if (!player) {
        QWidget::keyPressEvent(event);
        return;
    }

    switch (event->key()) {
    case Qt::Key_Escape:
    case Qt::Key_F:
        close();
        return;

    case Qt::Key_Space:
    case Qt::Key_K:
        player->togglePlayPause();
        break;

    case Qt::Key_Left:
        if (player->isSeekable())
            player->seek(player->position() - ArrowSeekMs);
        break;

    case Qt::Key_Right:
        if (player->isSeekable())
            player->seek(player->position() + ArrowSeekMs);
        break;

    case Qt::Key_Up:
        player->setMuted(false);
        player->setVolume(player->volume() + VolumeStep);
        break;

    case Qt::Key_Down:
        player->setVolume(player->volume() - VolumeStep);
        break;

    case Qt::Key_M:
        player->setMuted(!player->isMuted());
        break;

    default:
        QWidget::keyPressEvent(event);
        return;
    }

    revealControls();
    update();
}

void VideoFullscreenWindow::resizeEvent(QResizeEvent *event)
{
    if (player)
        player->setTargetSize(decodeSize());
    QWidget::resizeEvent(event);
}

void VideoFullscreenWindow::closeEvent(QCloseEvent *event)
{
    detach();
    unsetCursor();
    emit closed();
    QWidget::closeEvent(event);
}

} // namespace UI
} // namespace Acheron
