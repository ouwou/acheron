#include "VideoSurface.hpp"

#include "GlVideoLayer.hpp"
#include "VideoFrameStore.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QPen>
#include <QSettings>
#include <QWheelEvent>

#include <cmath>
#include <utility>

namespace Acheron {
namespace UI {

namespace {

const char *const GPU_DRAWING_KEY = "video/gpu_drawing";

constexpr int RESCALE_DELAY_MS = 120;

bool openGlWorks()
{
    static const bool works = [] {
        if (QGuiApplication::platformName() == QLatin1String("offscreen"))
            return false;
        QOpenGLContext context;
        return context.create();
    }();
    return works;
}

} // namespace

bool VideoSurface::gpuDrawingEnabled()
{
    return QSettings().value(GPU_DRAWING_KEY, true).toBool();
}

void VideoSurface::setGpuDrawingEnabled(bool enabled)
{
    QSettings().setValue(GPU_DRAWING_KEY, enabled);
}

VideoSurface::VideoSurface(QWidget *parent)
    : QWidget(parent), usesGpu(gpuDrawingEnabled() && openGlWorks())
{
    frameSink = std::make_shared<VideoFrameStore>(this, usesGpu);

    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 90);

    rescaleTimer.setSingleShot(true);
    rescaleTimer.setInterval(RESCALE_DELAY_MS);
    connect(&rescaleTimer, &QTimer::timeout, this, [this] {
        frameSink->rescale();
        refresh();
    });

    notShownTimer.setSingleShot(true);
    notShownTimer.setInterval(NOT_SHOWN_BEFORE_RELEASED_MS);
    connect(&notShownTimer, &QTimer::timeout, this, &VideoSurface::releaseWhatIsNotShown);
}

void VideoSurface::makeLayerForAPicture()
{
    if (glLayer || !usesGpu || !isVisible() || !frameSink->hasPicture())
        return;
    glLayer = new GlVideoLayer(this, frameSink);
    glContainer = QWidget::createWindowContainer(glLayer, this);
    glContainer->setFocusPolicy(Qt::NoFocus);
    glContainer->setAttribute(Qt::WA_TransparentForMouseEvents);
    glContainer->setGeometry(rect());
    glContainer->show();
    refreshCursor();
}

void VideoSurface::dropLayer()
{
    if (!glContainer)
        return;
    delete glContainer;
    glContainer = nullptr;
    glLayer = nullptr;
}

void VideoSurface::releaseWhatIsNotShown()
{
    if (isVisible())
        return;
    dropLayer();
    frameSink->keepInMemory();
}

VideoSurface::~VideoSurface()
{
    frameSink->detach();
    // now, while the rest of this still exists for it
    delete glContainer;
}

QImage VideoSurface::snapshot()
{
    return glLayer ? glLayer->snapshot() : grab().toImage();
}

std::shared_ptr<Core::Media::VideoFrameSink> VideoSurface::sink() const
{
    return frameSink;
}

bool VideoSurface::drawsFromCard() const
{
    return glLayer && glLayer->drawsFromCard();
}

void VideoSurface::refresh()
{
    if (glLayer)
        glLayer->update();
    else
        update();
}

void VideoSurface::refreshOverlays()
{
    if (glLayer)
        glLayer->invalidateOverlays();
    refresh();
}

void VideoSurface::setMessage(const QString &title, const QString &detail)
{
    if (title == messageTitle && detail == messageDetail)
        return;
    messageTitle = title;
    messageDetail = detail;
    refreshOverlays();
}

void VideoSurface::setStatsText(const QString &text)
{
    if (text == statsText)
        return;
    statsText = text;
    refreshOverlays();
}

void VideoSurface::setCaption(const QString &text)
{
    if (text == captionText)
        return;
    captionText = text;
    refreshOverlays();
}

void VideoSurface::setPlaceholder(const QPixmap &avatar)
{
    if (avatar.cacheKey() == placeholder.cacheKey())
        return;
    placeholder = avatar;
    refreshOverlays();
}

void VideoSurface::setSpeaking(bool value)
{
    if (speaking == value)
        return;
    speaking = value;
    refresh();
}

void VideoSurface::announceGeometry()
{
    reportGeometry(true);
}

void VideoSurface::clear()
{
    frameSink->forget();
    dropLayer();
    resetZoom();
    refresh();
}

void VideoSurface::resetZoom()
{
    zoom = 1.0;
    pan = {};
    dragStart.reset();
    applyZoom();
}

QSizeF VideoSurface::fittedPictureSize() const
{
    return QSizeF(frameSink->pictureSize()) / devicePixelRatioF();
}

QRectF VideoSurface::placedPicture(QSizeF fittedSize) const
{
    const QSizeF zoomed = fittedSize * zoom;
    const QPointF centred((width() - zoomed.width()) / 2.0, (height() - zoomed.height()) / 2.0);
    return QRectF(centred + pan, zoomed);
}

void VideoSurface::zoomTo(qreal wanted, QPointF keptStill)
{
    wanted = qBound(1.0, wanted, MAX_ZOOM);
    if (qFuzzyCompare(wanted, zoom))
        return;
    const QPointF middle(width() / 2.0, height() / 2.0);
    const QPointF pictureCentre = middle + pan;
    pan = keptStill - (keptStill - pictureCentre) * (wanted / zoom) - middle;
    zoom = wanted;
    applyZoom();
}

void VideoSurface::keepPanInside()
{
    const QSizeF zoomed = fittedPictureSize() * zoom;
    const qreal spareX = qMax(0.0, (zoomed.width() - width()) / 2.0);
    const qreal spareY = qMax(0.0, (zoomed.height() - height()) / 2.0);
    pan = QPointF(qBound(-spareX, pan.x(), spareX), qBound(-spareY, pan.y(), spareY));
}

void VideoSurface::applyZoom()
{
    if (zoom <= 1.0) {
        zoom = 1.0;
        pan = {};
    }
    keepPanInside();
    frameSink->setZoom(zoom);
    rescaleTimer.start();
    refreshCursor();
    refresh();
}

void VideoSurface::refreshCursor()
{
    const Qt::CursorShape shape = dragged ? Qt::ClosedHandCursor : Qt::OpenHandCursor;
    if (!isZoomed()) {
        unsetCursor();
        if (glLayer)
            glLayer->unsetCursor();
        return;
    }
    setCursor(shape);
    if (glLayer)
        glLayer->setCursor(shape);
}

void VideoSurface::onFrameArrived()
{
    makeLayerForAPicture();
    if (!isVisible())
        notShownTimer.start();
    refresh();
    reportGeometry();
}

void VideoSurface::reportGeometry(bool evenIfUnchanged)
{
    const QSize picture = frameSink->pictureSize();
    const QSize area = picture.isEmpty() ? size() * devicePixelRatioF() : picture;
    const int pixels = area.width() * area.height();
    const bool visible = isVisible() && !window()->isMinimized();
    if (!evenIfUnchanged && pixels == reportedPixels && visible == reportedVisible)
        return;
    reportedPixels = pixels;
    reportedVisible = visible;
    emit geometryChanged(pixels, visible);
}

void VideoSurface::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (glContainer)
        glContainer->setGeometry(rect());
    frameSink->setTarget(size() * devicePixelRatioF(), devicePixelRatioF());
    keepPanInside();
    rescaleTimer.start();
    reportGeometry();
}

void VideoSurface::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    notShownTimer.stop();
    frameSink->setShown(true);
    frameSink->setTarget(size() * devicePixelRatioF(), devicePixelRatioF());
    makeLayerForAPicture();
    reportGeometry();
}

void VideoSurface::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    frameSink->setShown(false);
    notShownTimer.start();
    reportedPixels = 0;
    reportedVisible = false;
    emit geometryChanged(0, false);
}

void VideoSurface::mouseDoubleClickEvent(QMouseEvent *event)
{
    QWidget::mouseDoubleClickEvent(event);
    emit doubleClicked();
}

void VideoSurface::mousePressEvent(QMouseEvent *event)
{
    QWidget::mousePressEvent(event);
    if (event->button() != Qt::LeftButton || !isZoomed())
        return;
    dragStart = event->pos();
    panAtDragStart = pan;
    dragged = false;
}

void VideoSurface::mouseMoveEvent(QMouseEvent *event)
{
    QWidget::mouseMoveEvent(event);
    if (!dragStart)
        return;
    const QPoint moved = event->pos() - *dragStart;
    if (!dragged && moved.manhattanLength() < QApplication::startDragDistance())
        return;
    if (!dragged) {
        dragged = true;
        refreshCursor();
    }
    pan = panAtDragStart + moved;
    keepPanInside();
    refresh();
}

void VideoSurface::mouseReleaseEvent(QMouseEvent *event)
{
    QWidget::mouseReleaseEvent(event);
    if (event->button() != Qt::LeftButton)
        return;
    const bool wasDrag = std::exchange(dragged, false);
    dragStart.reset();
    refreshCursor();
    if (!wasDrag && rect().contains(event->pos()))
        emit clicked();
}

void VideoSurface::wheelEvent(QWheelEvent *event)
{
    const int eighthsOfADegree = event->angleDelta().y();
    if (eighthsOfADegree == 0 || !frameSink->hasPicture()) {
        QWidget::wheelEvent(event);
        return;
    }
    constexpr qreal EIGHTHS_PER_STEP = 120.0;
    zoomTo(zoom * std::pow(ZOOM_PER_WHEEL_STEP, eighthsOfADegree / EIGHTHS_PER_STEP), event->position());
    event->accept();
}

void VideoSurface::paintEvent(QPaintEvent *)
{
    if (glLayer)
        return;

    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    bool hasPicture = false;
    frameSink->withFront([&](const QImage &image) {
        if (image.isNull())
            return;
        hasPicture = true;
        const QRectF source(QPointF(0, 0), QSizeF(image.size()));
        if (isZoomed()) {
            painter.drawImage(placedPicture(fittedPictureSize()), image, source);
            return;
        }
        const qreal ratio = devicePixelRatioF();
        const QSize widgetPixels = size() * ratio;
        if (image.size() == fitted(image.size(), widgetPixels)) {
            // on whole device pixels, so it is copied, not resampled
            const QPoint offset((widgetPixels.width() - image.width()) / 2, (widgetPixels.height() - image.height()) / 2);
            painter.drawImage(QPointF(offset) / ratio, image);
            return;
        }
        const QSizeF shown = QSizeF(image.size()).scaled(QSizeF(size()), Qt::KeepAspectRatio);
        const QRectF where(QPointF((width() - shown.width()) / 2.0, (height() - shown.height()) / 2.0), shown);
        painter.drawImage(where, image, source);
    });

    paintOverlays(painter, hasPicture, true);
}

bool VideoSurface::hasOverlaysBesideTheRing(bool hasPicture) const
{
    return (!hasPicture && !placeholder.isNull()) || !messageTitle.isEmpty() || !captionText.isEmpty() || !statsText.isEmpty();
}

void VideoSurface::paintOverlays(QPainter &painter, bool hasPicture, bool withSpeakingRing)
{
    if (!hasPicture && !placeholder.isNull()) {
        const int side = qBound(24, qMin(width(), height()) / 3, 128);
        const QRect where((width() - side) / 2, (height() - side) / 2, side, side);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(where, placeholder);
    }

    if (!messageTitle.isEmpty()) {
        painter.fillRect(rect(), QColor(0, 0, 0, 160));
        painter.setPen(Qt::white);

        QFont titleFont = font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.4);
        painter.setFont(titleFont);
        const QRect titleArea = rect().adjusted(16, 0, -16, -rect().height() / 2);
        painter.drawText(titleArea, Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, messageTitle);

        painter.setFont(font());
        const QRect detailArea = rect().adjusted(16, rect().height() / 2 + 6, -16, 0);
        painter.drawText(detailArea, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, messageDetail);
    }

    if (!captionText.isEmpty()) {
        painter.setFont(font());
        const QRect bounds = fontMetrics().boundingRect(rect().adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignBottom, captionText);
        painter.fillRect(bounds.adjusted(-4, -2, 4, 2), QColor(0, 0, 0, 170));
        painter.setPen(Qt::white);
        painter.drawText(bounds, Qt::AlignLeft | Qt::AlignBottom, captionText);
    }

    if (speaking && withSpeakingRing) {
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(speakingRingColor(), 2));
        painter.drawRect(rect().adjusted(1, 1, -1, -1));
    }

    if (!statsText.isEmpty()) {
        const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        painter.setFont(fixed);
        const QRect bounds = QFontMetrics(fixed, this).boundingRect(rect().adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignTop, statsText);
        if (bounds.height() <= height() / 2 && bounds.width() <= width() - 16) {
            painter.fillRect(bounds.adjusted(-4, -4, 4, 4), QColor(0, 0, 0, 170));
            painter.setPen(Qt::white);
            painter.drawText(bounds, Qt::AlignLeft | Qt::AlignTop, statsText);
        }
    }
}

} // namespace UI
} // namespace Acheron
