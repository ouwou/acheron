#include "UI/Call/CallTile.hpp"

#include "Core/Streams/StreamSession.hpp"
#include "Core/Video/CallVideo.hpp"
#include "UI/Video/VideoSurface.hpp"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>

extern "C" {
#include <libavutil/frame.h>
}

namespace Acheron {
namespace UI {

namespace {

constexpr QSize TILE_WINDOW_SIZE(640, 360);

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
        if (togglesFullScreen(event, isFullScreen()))
            toggleFullScreen();
        else
            QWidget::keyPressEvent(event);
    }

private:
    std::function<void()> closed;
};

} // namespace

bool togglesFullScreen(const QKeyEvent *event, bool fullScreen)
{
    return (event->key() == Qt::Key_Escape && fullScreen) || event->key() == Qt::Key_F11 || event->key() == Qt::Key_F;
}

void paintBlack(QWidget *widget)
{
    QPalette black = widget->palette();
    black.setColor(QPalette::Window, Qt::black);
    widget->setPalette(black);
    widget->setAutoFillBackground(true);
}

CallTile::CallTile(const TileKey &key, QWidget *stage, Core::CallVideo *video) : tileKey(key), drawnOn(new VideoSurface(stage)), video(video)
{
    drawnOn->setMinimumSize(1, 1);
    drawnOn->setContextMenuPolicy(Qt::CustomContextMenu);
    drawnOn->show();
}

CallTile::~CallTile()
{
    if (camera && video)
        video->removeTile(tileKey.userId);
    if (watched)
        watched->setSurface(0, false);
    // deleting hides the surface, which it would announce to whoever still listens
    drawnOn->disconnect();
    delete drawnOn;
    delete window.data();
}

void CallTile::bind(Core::StreamSession *session)
{
    watched = session;
    drawnOn->clear();
    if (!session)
        return;
    session->setSink(drawnOn->sink());
    drawnOn->announceGeometry();
}

void CallTile::setHasCamera(bool on)
{
    if (camera == on)
        return;
    camera = on;
    if (on) {
        drawnOn->announceGeometry();
        return;
    }
    if (video)
        video->removeTile(tileKey.userId);
    drawnOn->clear();
}

void CallTile::announceGeometry(int pixelCount, bool visible)
{
    if (watched)
        watched->setSurface(pixelCount, visible);
    else if (camera && video)
        video->setTile(tileKey.userId, drawnOn->sink(), pixelCount, visible);
}

void CallTile::present(const QImage &picture)
{
    const QImage pixels = picture.convertToFormat(QImage::Format_RGB32);
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return;

    frame->format = AV_PIX_FMT_RGB32;
    frame->width = pixels.width();
    frame->height = pixels.height();
    // what swscale assumes when it makes YUV of this for the shader
    frame->colorspace = AVCOL_SPC_SMPTE170M;
    frame->color_range = AVCOL_RANGE_MPEG;
    if (av_frame_get_buffer(frame, 0) == 0) {
        const size_t rowBytes = static_cast<size_t>(pixels.width()) * 4;
        for (int row = 0; row < pixels.height(); row++)
            std::memcpy(frame->data[0] + static_cast<qptrdiff>(row) * frame->linesize[0], pixels.constScanLine(row), rowBytes);
        drawnOn->sink()->present(frame);
    }
    av_frame_free(&frame);
}

void CallTile::popOut(const QString &title, std::function<void()> closed)
{
    if (window)
        return;
    auto *tileWindow = new TileWindow(drawnOn, title, std::move(closed));
    window = tileWindow;
    drawnOn->show();
    tileWindow->show();
}

void CallTile::dockIn(QWidget *stage)
{
    if (!window)
        return;
    QWidget *leaving = window;
    window = nullptr;
    drawnOn->setParent(stage);
    drawnOn->show();
    leaving->hide();
    leaving->deleteLater();
}

void CallTile::toggleFullScreen()
{
    if (window)
        static_cast<TileWindow *>(window.data())->toggleFullScreen();
}

} // namespace UI
} // namespace Acheron
