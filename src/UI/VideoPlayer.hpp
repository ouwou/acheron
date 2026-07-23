#pragma once

#include <QWidget>
#include <QUrl>

class QMediaPlayer;
class QVideoWidget;
class QSlider;
class QPushButton;
class QToolButton;
class QLabel;

namespace Acheron {
namespace UI {

class VideoPlayer : public QWidget
{
    Q_OBJECT
public:
    explicit VideoPlayer(QWidget *parent = nullptr);
    ~VideoPlayer();

    void showVideo(const QUrl &url);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onPlayPauseClicked();
    void onPositionChanged(qint64 position);
    void onDurationChanged(qint64 duration);
    void onSeekPressed();
    void onSeekReleased();
    void onVolumeMoved(int volume);
    void onStateChanged(int state);
    void onFullScreenClicked();
    void hideControls();

private:
    void updateGeometryToParent();
    void setupControls();
    QString formatTime(qint64 ms) const;
    void setControlsVisible(bool visible);
    void updatePlayPauseIcon();
    void updateTimeDisplay();

    QMediaPlayer *mediaPlayer = nullptr;
    QVideoWidget *videoWidget = nullptr;

    QWidget *controlBar = nullptr;
    QPushButton *playPauseButton = nullptr;
    QSlider *seekSlider = nullptr;
    QLabel *timeLabel = nullptr;
    QToolButton *volumeButton = nullptr;
    QSlider *volumeSlider = nullptr;
    QToolButton *fullScreenButton = nullptr;

    bool controlsVisible = true;
    bool isDraggingSlider = false;
    qint64 lastSeekTime = 0;

    QWidget *trackedWindow = nullptr;
    QUrl currentUrl;
};

} // namespace UI
} // namespace Acheron