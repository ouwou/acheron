#pragma once

#include <QColor>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <memory>

#include "Core/Media/VideoDecoder.hpp"

class QPainter;

namespace Acheron {
namespace UI {

class GlVideoLayer;
class VideoFrameStore;

// Drawn over video, so it does not follow the theme: the green Discord rings a speaker with.
inline const QColor SPEAKING_RING(0x23, 0xA5, 0x5A);

// Shows one decoded video. Where the machine can, the picture is converted and scaled by the
// graphics card, from textures it is uploaded as or, when the decoder left it on the card,
// from where it already is. Otherwise it is converted and scaled to the widget's size on the
// decoder's thread, so that painting is a copy of pixels already at the right size.
class VideoSurface : public QWidget
{
    Q_OBJECT
public:
    explicit VideoSurface(QWidget *parent = nullptr);
    ~VideoSurface() override;

    // Whether new surfaces draw through the graphics card when they can. A setting; on unless
    // the user turns it off.
    [[nodiscard]] static bool gpuDrawingEnabled();
    static void setGpuDrawingEnabled(bool enabled);

    [[nodiscard]] std::shared_ptr<Core::Media::VideoFrameSink> sink() const;
    // Whether this surface draws its pictures through the graphics card.
    [[nodiscard]] bool drawsWithGpu() const { return usesGpu; }
    // Whether the picture now showing went from the decoder to the screen without leaving the
    // graphics card.
    [[nodiscard]] bool drawsFromCard() const;

    // Text over the picture: a state such as "Stream Paused" in the middle, and statistics in
    // the corner. Empty strings clear them.
    void setMessage(const QString &title, const QString &detail = {});
    void setStatsText(const QString &text);
    // A short label in the bottom corner, such as whose video this is.
    void setCaption(const QString &text);
    [[nodiscard]] QString caption() const { return captionText; }
    // What stands in for the picture while there is none: the person's avatar.
    void setPlaceholder(const QPixmap &avatar);
    // A ring around the tile while the person is speaking.
    void setSpeaking(bool speaking);

    // Sends geometryChanged again, for a listener that was not there the first time.
    void announceGeometry();

    // Forgets the picture, so that a stream which is over does not leave its last frame up.
    void clear();

    // What the surface is showing, picture and everything over it. With the graphics card
    // the picture is in a window of its own, which grab() does not see. Null until shown.
    [[nodiscard]] QImage snapshot();

    // A surface that has been off the screen, and had no new picture, for this long gives up
    // what it holds on the graphics card. Shorter, and switching away and back would cost a
    // moment without its picture every time.
    static constexpr int NOT_SHOWN_BEFORE_RELEASED_MS = 10'000;

signals:
    // The area the picture covers, in device pixels, and whether any of it is on screen.
    void geometryChanged(int pixelCount, bool visible);
    void clicked();
    void doubleClicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

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
    // Everything drawn on top of the picture, the same whichever way the picture is drawn.
    // The layer for the graphics card draws the speaking ring by itself.
    void paintOverlays(QPainter &painter, bool hasPicture, bool withSpeakingRing);
    [[nodiscard]] bool hasOverlaysBesideTheRing(bool hasPicture) const;

    std::shared_ptr<VideoFrameStore> frameSink;
    bool usesGpu = false;
    // The layer is a window with an OpenGL context of its own. A surface has one while it has
    // a picture to draw there, which a tile showing an avatar never does.
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
};

} // namespace UI
} // namespace Acheron
