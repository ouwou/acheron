#pragma once

#include <QColor>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <memory>
#include <optional>

#include "Core/Media/VideoDecoder.hpp"

class QPainter;

namespace Acheron {
namespace UI {

class GlVideoLayer;
class VideoFrameStore;

// drawn over video, so it doesnt follow the theme
inline const QColor SPEAKING_RING(0x23, 0xA5, 0x5A);

class VideoSurface : public QWidget
{
    Q_OBJECT
public:
    explicit VideoSurface(QWidget *parent = nullptr);
    ~VideoSurface() override;

    [[nodiscard]] static bool gpuDrawingEnabled();
    static void setGpuDrawingEnabled(bool enabled);

    [[nodiscard]] std::shared_ptr<Core::Media::VideoFrameSink> sink() const;
    [[nodiscard]] bool drawsWithGpu() const { return usesGpu; }
    [[nodiscard]] bool drawsFromCard() const;

    void setMessage(const QString &title, const QString &detail = {});
    void setStatsText(const QString &text);
    void setCaption(const QString &text);
    [[nodiscard]] QString caption() const { return captionText; }
    void setPlaceholder(const QPixmap &avatar);
    void setSpeaking(bool speaking);

    void announceGeometry();

    void clear();

    [[nodiscard]] bool isZoomed() const { return zoom > 1.0; }
    void resetZoom();

    // through the card the picture is in a window of its own, which grab() doesnt see
    [[nodiscard]] QImage snapshot();

    // any shorter and every switch away and back would cost a moment without a picture
    static constexpr int NOT_SHOWN_BEFORE_RELEASED_MS = 10'000;
    static constexpr qreal MAX_ZOOM = 8.0;
    static constexpr qreal ZOOM_PER_WHEEL_STEP = 1.25;

signals:
    void geometryChanged(int pixelCount, bool visible);
    void clicked();
    void doubleClicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    friend class GlVideoLayer;
    friend class VideoFrameStore;

    void onFrameArrived();
    void reportGeometry(bool evenIfUnchanged = false);
    void refresh();
    void refreshOverlays();
    void makeLayerForAPicture();
    void dropLayer();
    void releaseWhatIsNotShown();
    void paintOverlays(QPainter &painter, bool hasPicture, bool withSpeakingRing);
    [[nodiscard]] bool hasOverlaysBesideTheRing(bool hasPicture) const;
    [[nodiscard]] QSizeF fittedPictureSize() const;
    // in this widget's coordinates, past its edges when zoomed in
    [[nodiscard]] QRectF placedPicture(QSizeF fittedSize) const;
    void zoomTo(qreal wanted, QPointF keptStill);
    void keepPanInside();
    void applyZoom();
    void refreshCursor();

    std::shared_ptr<VideoFrameStore> frameSink;
    bool usesGpu = false;
    GlVideoLayer *glLayer = nullptr;
    QWidget *glContainer = nullptr;
    QTimer rescaleTimer;
    QTimer notShownTimer;
    QString messageTitle;
    QString messageDetail;
    QString statsText;
    QString captionText;
    QPixmap placeholder;
    bool speaking = false;
    int reportedPixels = -1;
    bool reportedVisible = false;
    qreal zoom = 1.0;
    // of the picture's centre from this widget's
    QPointF pan;
    std::optional<QPoint> dragStart;
    QPointF panAtDragStart;
    bool dragged = false;
};

} // namespace UI
} // namespace Acheron
