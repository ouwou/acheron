#include "VideoSurface.hpp"

#include "GlVideoLayer.hpp"
#include "VideoFrameStore.hpp"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QPen>
#include <QSettings>

namespace Acheron {
namespace UI {

namespace {

const char *const GPU_DRAWING_KEY = "video/gpu_drawing";

constexpr int RESCALE_DELAY_MS = 120;

// remote sessions and some old machines have none
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
    refresh();
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

void VideoSurface::mouseReleaseEvent(QMouseEvent *event)
{
    QWidget::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && rect().contains(event->pos()))
        emit clicked();
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
        painter.drawImage(where, image, QRectF(QPointF(0, 0), QSizeF(image.size())));
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
        // the painter may be on the GL framebuffer, whose font sizes are unreliable
        painter.setFont(font());
        const QRect bounds = fontMetrics().boundingRect(rect().adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignBottom, captionText);
        painter.fillRect(bounds.adjusted(-4, -2, 4, 2), QColor(0, 0, 0, 170));
        painter.setPen(Qt::white);
        painter.drawText(bounds, Qt::AlignLeft | Qt::AlignBottom, captionText);
    }

    if (speaking && withSpeakingRing) {
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(SPEAKING_RING, 2));
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
