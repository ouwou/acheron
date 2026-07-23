#include "VideoPlayer.hpp"

#include <QMediaPlayer>
#include <QSlider>
#include <QPushButton>
#include <QToolButton>
#include <QDateTime>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QApplication>
#include <QPainter>
#include <QStyle>
#include <QAudioOutput>
#include <QVideoWidget>
#include <QPixmap>
#include <QIcon>

namespace Acheron {
namespace UI {

static QIcon whiteIcon(QStyle::StandardPixmap sp, QWidget *w)
{
    QIcon icon = w->style()->standardIcon(sp);
    QPixmap px = icon.pixmap(48);
    if (px.isNull())
        return icon;
    QPainter p(&px);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(px.rect(), Qt::white);
    p.end();
    return QIcon(px);
}

VideoPlayer::VideoPlayer(QWidget *parent) : QWidget(parent)
{
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    mediaPlayer = new QMediaPlayer(this);

    videoWidget = new QVideoWidget(this);
    videoWidget->setAspectRatioMode(Qt::KeepAspectRatio);
    mediaPlayer->setVideoOutput(videoWidget);

    setupControls();

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    mainLayout->addWidget(videoWidget);
    mainLayout->addWidget(controlBar);

#if QT_VERSION_MAJOR >= 6
    connect(mediaPlayer, &QMediaPlayer::playbackStateChanged, this, &VideoPlayer::onStateChanged);
    connect(mediaPlayer, &QMediaPlayer::errorOccurred, this, [](QMediaPlayer::Error) {});
#else
    connect(mediaPlayer, static_cast<void(QMediaPlayer::*)(QMediaPlayer::State)>(&QMediaPlayer::stateChanged),
            this, &VideoPlayer::onStateChanged);
    connect(mediaPlayer, static_cast<void(QMediaPlayer::*)(QMediaPlayer::Error)>(&QMediaPlayer::error),
            this, [](QMediaPlayer::Error) {});
#endif
    connect(mediaPlayer, &QMediaPlayer::positionChanged, this, &VideoPlayer::onPositionChanged);
    connect(mediaPlayer, &QMediaPlayer::durationChanged, this, &VideoPlayer::onDurationChanged);
}

VideoPlayer::~VideoPlayer() = default;

void VideoPlayer::setupControls()
{
    controlBar = new QWidget(this);
    controlBar->setFixedHeight(60);
    controlBar->setStyleSheet("background-color: rgba(30, 30, 30, 220);");

    auto *layout = new QHBoxLayout(controlBar);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(12);

    playPauseButton = new QPushButton(this);
    playPauseButton->setIcon(whiteIcon(QStyle::SP_MediaPlay, this));
    playPauseButton->setIconSize(QSize(24, 24));
    playPauseButton->setFixedSize(32, 32);
    playPauseButton->setFlat(true);
    layout->addWidget(playPauseButton);
    connect(playPauseButton, &QPushButton::clicked, this, &VideoPlayer::onPlayPauseClicked);

    seekSlider = new QSlider(Qt::Horizontal, this);
    seekSlider->setRange(0, 0);
    layout->addWidget(seekSlider);
    connect(seekSlider, &QSlider::sliderPressed, this, [this]() {
        isDraggingSlider = true;
    });
    connect(seekSlider, &QSlider::sliderMoved, this, [this](int pos) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastSeekTime >= 100) {
            lastSeekTime = now;
            mediaPlayer->setPosition(pos);
        }
    });
    connect(seekSlider, &QSlider::sliderReleased, this, [this]() {
        isDraggingSlider = false;
        mediaPlayer->setPosition(seekSlider->value());
    });

    timeLabel = new QLabel("0:00 / 0:00", this);
    timeLabel->setStyleSheet("color: white; font-size: 12px;");
    layout->addWidget(timeLabel);

    volumeButton = new QToolButton(this);
    volumeButton->setIcon(whiteIcon(QStyle::SP_MediaVolume, this));
    volumeButton->setIconSize(QSize(20, 20));
    volumeButton->setFixedSize(28, 28);
    volumeButton->setAutoRaise(true);
    layout->addWidget(volumeButton);

    volumeSlider = new QSlider(Qt::Horizontal, this);
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(100);
    volumeSlider->setFixedWidth(80);
    layout->addWidget(volumeSlider);
    connect(volumeSlider, &QSlider::valueChanged, this, &VideoPlayer::onVolumeMoved);

    layout->addStretch();

    fullScreenButton = new QToolButton(this);
    fullScreenButton->setIcon(whiteIcon(QStyle::SP_TitleBarMaxButton, this));
    fullScreenButton->setIconSize(QSize(20, 20));
    fullScreenButton->setFixedSize(28, 28);
    fullScreenButton->setAutoRaise(true);
    layout->addWidget(fullScreenButton);
    connect(fullScreenButton, &QToolButton::clicked, this, &VideoPlayer::onFullScreenClicked);

    auto *audioOutput = new QAudioOutput(this);
    mediaPlayer->setAudioOutput(audioOutput);
    audioOutput->setVolume(1.0);
}

void VideoPlayer::showVideo(const QUrl &url)
{
    currentUrl = url;

    if (parentWidget()) {
        trackedWindow = parentWidget()->window();
        trackedWindow->installEventFilter(this);
    }

    updateGeometryToParent();

    show();
    raise();
    activateWindow();
    setFocus();

#if QT_VERSION_MAJOR >= 6
    mediaPlayer->setSource(url);
#else
    mediaPlayer->setMedia(url);
#endif

    mediaPlayer->play();
}

void VideoPlayer::updateGeometryToParent()
{
    if (trackedWindow)
        setGeometry(trackedWindow->geometry());
    else
        showFullScreen();
}

void VideoPlayer::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0, 0, 0, 180));
}

void VideoPlayer::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        QRect safeRect = videoWidget->geometry().united(controlBar->geometry());
        if (!safeRect.contains(event->pos())) {
            close();
            return;
        }
        setControlsVisible(!controlsVisible);
    }
    QWidget::mousePressEvent(event);
}

void VideoPlayer::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        if (isFullScreen()) {
            showNormal();
            fullScreenButton->setIcon(whiteIcon(QStyle::SP_TitleBarMaxButton, this));
        } else {
            close();
        }
        break;
    case Qt::Key_Space:
        onPlayPauseClicked();
        break;
    case Qt::Key_F:
        onFullScreenClicked();
        break;
    case Qt::Key_R:
        mediaPlayer->setPosition(0);
        break;
    default:
        QWidget::keyPressEvent(event);
    }
}

void VideoPlayer::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event);
}

bool VideoPlayer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == trackedWindow) {
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize) {
            updateGeometryToParent();
        } else if (event->type() == QEvent::Close) {
            close();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void VideoPlayer::onPlayPauseClicked()
{
#if QT_VERSION_MAJOR >= 6
    if (mediaPlayer->playbackState() == QMediaPlayer::PlayingState)
        mediaPlayer->pause();
    else
        mediaPlayer->play();
#else
    if (mediaPlayer->state() == QMediaPlayer::PlayingState)
        mediaPlayer->pause();
    else
        mediaPlayer->play();
#endif
}

void VideoPlayer::onPositionChanged(qint64 position)
{
    if (!isDraggingSlider && !seekSlider->isSliderDown()) {
        seekSlider->blockSignals(true);
        seekSlider->setValue(static_cast<int>(position));
        seekSlider->blockSignals(false);
    }
    updateTimeDisplay();
}

void VideoPlayer::onDurationChanged(qint64 duration)
{
    seekSlider->setRange(0, static_cast<int>(duration));
    updateTimeDisplay();
}

void VideoPlayer::onSeekPressed()
{
    isDraggingSlider = true;
}

void VideoPlayer::onSeekReleased()
{
}

void VideoPlayer::onVolumeMoved(int volume)
{
    if (auto *audio = mediaPlayer->audioOutput())
        audio->setVolume(volume / 100.0);
    if (volume == 0)
        volumeButton->setIcon(whiteIcon(QStyle::SP_MediaVolumeMuted, this));
    else
        volumeButton->setIcon(whiteIcon(QStyle::SP_MediaVolume, this));
}

void VideoPlayer::onStateChanged(int state)
{
    Q_UNUSED(state);
    updatePlayPauseIcon();
}

void VideoPlayer::onFullScreenClicked()
{
    if (isFullScreen()) {
        showNormal();
        fullScreenButton->setIcon(whiteIcon(QStyle::SP_TitleBarMaxButton, this));
    } else {
        showFullScreen();
        fullScreenButton->setIcon(whiteIcon(QStyle::SP_TitleBarNormalButton, this));
    }
}

void VideoPlayer::hideControls()
{
    setControlsVisible(false);
}

void VideoPlayer::setControlsVisible(bool visible)
{
    if (controlsVisible == visible)
        return;
    controlsVisible = visible;
    controlBar->setVisible(visible);
}

void VideoPlayer::updatePlayPauseIcon()
{
#if QT_VERSION_MAJOR >= 6
    if (mediaPlayer->playbackState() == QMediaPlayer::PlayingState)
        playPauseButton->setIcon(whiteIcon(QStyle::SP_MediaPause, this));
    else
        playPauseButton->setIcon(whiteIcon(QStyle::SP_MediaPlay, this));
#else
    if (mediaPlayer->state() == QMediaPlayer::PlayingState)
        playPauseButton->setIcon(whiteIcon(QStyle::SP_MediaPause, this));
    else
        playPauseButton->setIcon(whiteIcon(QStyle::SP_MediaPlay, this));
#endif
}

void VideoPlayer::updateTimeDisplay()
{
    qint64 position = mediaPlayer->position();
    qint64 duration = mediaPlayer->duration();
    timeLabel->setText(formatTime(position) + " / " + formatTime(duration));
}

QString VideoPlayer::formatTime(qint64 ms) const
{
    if (ms < 0) ms = 0;
    qint64 seconds = ms / 1000;
    qint64 minutes = seconds / 60;
    seconds %= 60;
    if (minutes >= 60) {
        qint64 hours = minutes / 60;
        minutes %= 60;
        return QString("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

} // namespace UI
} // namespace Acheron