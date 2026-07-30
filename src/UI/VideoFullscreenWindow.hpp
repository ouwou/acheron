#pragma once

#include <QPointer>
#include <QSize>
#include <QString>
#include <QWidget>

#include "UI/Chat/VideoControls.hpp"

class QTimer;

namespace Acheron {

namespace Core {
namespace Media {
class Player;
} // namespace Media
} // namespace Core

namespace UI {

class VideoFullscreenWindow : public QWidget
{
    Q_OBJECT
public:
    explicit VideoFullscreenWindow(QWidget *parent = nullptr);
    ~VideoFullscreenWindow() override;

    void showPlayer(Core::Media::Player *player, const QString &key, const QSize &inlineSize);

    [[nodiscard]] QString mediaKey() const { return key; }

signals:
    void closed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    [[nodiscard]] QSize decodeSize() const { return size() * devicePixelRatioF(); }
    [[nodiscard]] VideoControls::State controlState() const;
    void detach();
    void revealControls();

    QPointer<Core::Media::Player> player;
    QString key;
    QSize inlineSize;

    QTimer *idleTimer = nullptr;
    VideoControls::Drag drag;
    bool controlsVisible = true;
    bool volumeExpanded = false;
};

} // namespace UI
} // namespace Acheron
