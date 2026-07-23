#include "ImageViewer.hpp"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QApplication>

namespace Acheron {
namespace UI {

ImageViewer::ImageViewer(QWidget *parent) : QWidget(parent)
{
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
    setFocusPolicy(Qt::StrongFocus);

    networkManager = new QNetworkAccessManager(this);
}

void ImageViewer::showImage(const QUrl &proxyUrl, const QPixmap &preview)
{
    currentUrl = proxyUrl;
    currentImage = preview;
    fullImage = QPixmap();
    isLoadingFull = true;

    if (parentWidget()) {
        trackedWindow = parentWidget()->window();
        trackedWindow->installEventFilter(this);
    }

    updateGeometryToParent();

    show();
    raise();
    activateWindow();
    setFocus();

    resetView();
    fetchFullImage(proxyUrl);
}

void ImageViewer::updateGeometryToParent()
{
    if (trackedWindow)
        setGeometry(trackedWindow->geometry());
    else
        showFullScreen();
}

void ImageViewer::fetchFullImage(const QUrl &proxyUrl)
{
    QUrl fetchUrl = proxyUrl;
    QUrlQuery query(fetchUrl);
    query.addQueryItem("format", "webp");
    query.addQueryItem("quality", "lossless");
    fetchUrl.setQuery(query);

    QNetworkRequest request(fetchUrl);
    QNetworkReply *reply = networkManager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            isLoadingFull = false;
            update();
            return;
        }

        QByteArray data = reply->readAll();
        QPixmap pixmap;
        if (pixmap.loadFromData(data)) {
            qreal dpr = qApp->devicePixelRatio();
            pixmap.setDevicePixelRatio(dpr);

            if (!currentImage.isNull()) {
                QSizeF oldLogicalSize = currentImage.size() / currentImage.devicePixelRatio();
                QSizeF newLogicalSize = pixmap.size() / dpr;

                qreal scaleRatio = oldLogicalSize.width() / newLogicalSize.width();
                zoomLevel *= scaleRatio;
            }

            fullImage = pixmap;
            currentImage = fullImage;
        }

        isLoadingFull = false;
        update();
    });
}

void ImageViewer::resetView()
{
    zoomLevel = 1.0;
    panOffset = QPointF(0, 0);

    if (!currentImage.isNull()) {
        QSizeF imageSize = currentImage.size() / currentImage.devicePixelRatio();
        QSizeF windowSize = size();

        qreal scaleX = windowSize.width() / imageSize.width();
        qreal scaleY = windowSize.height() / imageSize.height();
        zoomLevel = qMin(scaleX, scaleY) * 0.8;
    }
}

void ImageViewer::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    painter.fillRect(rect(), QColor(0, 0, 0, 100));

    if (currentImage.isNull())
        return;

    QSizeF imageSize = currentImage.size() / currentImage.devicePixelRatio();
    QSizeF scaledSize = imageSize * zoomLevel;

    QPointF center = QPointF(width() / 2.0, height() / 2.0) + panOffset;
    QRectF destRect(center.x() - scaledSize.width() / 2.0, center.y() - scaledSize.height() / 2.0,
                    scaledSize.width(), scaledSize.height());

    painter.drawPixmap(destRect, currentImage, QRectF(QPointF(0, 0), currentImage.size()));

    if (isLoadingFull) {
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignTop | Qt::AlignHCenter,
                         "\n" + tr("Loading full resolution..."));
    }

    // help text
    painter.setPen(QColor(255, 255, 255, 150));
    painter.drawText(
            rect().adjusted(10, 0, -10, -10), Qt::AlignBottom | Qt::AlignHCenter,
            tr("Scroll to zoom  |  Drag to pan  |  Esc or click outside to close  |  R to reset"));
}

void ImageViewer::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        QSizeF imageSize = currentImage.size() / currentImage.devicePixelRatio();
        QSizeF scaledSize = imageSize * zoomLevel;
        QPointF center = QPointF(width() / 2.0, height() / 2.0) + panOffset;
        QRectF imageRect(center.x() - scaledSize.width() / 2.0,
                         center.y() - scaledSize.height() / 2.0, scaledSize.width(),
                         scaledSize.height());

        if (imageRect.contains(event->pos())) {
            isPanning = true;
            lastMousePos = event->pos();
            setCursor(Qt::ClosedHandCursor);
        } else {
            close();
        }
    }
}

void ImageViewer::mouseMoveEvent(QMouseEvent *event)
{
    if (isPanning) {
        QPointF delta = event->pos() - lastMousePos;
        panOffset += delta;
        lastMousePos = event->pos();
        update();
    }
}

void ImageViewer::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && isPanning) {
        isPanning = false;
        setCursor(Qt::OpenHandCursor);
    }
}

void ImageViewer::wheelEvent(QWheelEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QPointF mousePos = event->position();
#else
    QPointF mousePos = event->posF();
#endif
    QPointF beforeZoom = widgetToImage(mousePos);

    qreal factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    qreal newZoom = zoomLevel * factor;

    newZoom = qBound(0.1, newZoom, 20.0);
    zoomLevel = newZoom;

    QPointF afterZoom = widgetToImage(mousePos);
    QPointF correction = (afterZoom - beforeZoom) * zoomLevel;
    panOffset += correction;

    update();
}

void ImageViewer::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        close();
        break;
    case Qt::Key_R:
        resetView();
        update();
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomLevel *= 1.25;
        update();
        break;
    case Qt::Key_Minus:
        zoomLevel /= 1.25;
        update();
        break;
    default:
        QWidget::keyPressEvent(event);
    }
}

void ImageViewer::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event);
}

bool ImageViewer::eventFilter(QObject *watched, QEvent *event)
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

QPointF ImageViewer::imageToWidget(const QPointF &imagePoint) const
{
    QPointF center = QPointF(width() / 2.0, height() / 2.0) + panOffset;
    return center + imagePoint * zoomLevel;
}

QPointF ImageViewer::widgetToImage(const QPointF &widgetPoint) const
{
    QPointF center = QPointF(width() / 2.0, height() / 2.0) + panOffset;
    return (widgetPoint - center) / zoomLevel;
}

} // namespace UI
} // namespace Acheron
