#pragma once

#include <QMatrix3x3>
#include <QOpenGLFunctions>
#include <QOpenGLWindow>
#include <QVector2D>
#include <QVector3D>

#include <array>
#include <memory>

class QOpenGLShaderProgram;
struct AVFrame;

namespace Acheron {
namespace UI {

class CardPicture;
class DmabufGlBridge;
class VideoFrameStore;
class VideoSurface;

// Draws a surface's picture with the graphics card. A frame in memory goes up as textures, and
// a shader converts them to RGB while the card scales them to the widget. A frame the decoder
// left on the card arrives as a finished texture, which is only drawn, or, from VAAPI, as
// textures that are its planes where they lie. The layer sits inside the surface, fills it, and
// passes the mouse and keys on to it.
//
// It is a window of its own inside the surface's window, not a widget. A widget that draws
// with OpenGL makes Qt rebuild the whole window it is put into, visibly, and from then on
// compose all of that window through OpenGL for every frame of video.
class GlVideoLayer : public QOpenGLWindow, protected QOpenGLFunctions
{
public:
    GlVideoLayer(VideoSurface *owner, std::shared_ptr<VideoFrameStore> frames);
    ~GlVideoLayer() override;

    // The overlays have changed and need painting again.
    void invalidateOverlays() { overlaysStale = true; }
    [[nodiscard]] bool drawsFromCard() const { return shown == Shown::OnCard; }
    // What is on the surface now, drawn afresh. Null before the window has been shown.
    [[nodiscard]] QImage snapshot();

protected:
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void initializeGL() override;
    void paintGL() override;

private:
    enum class Shown {
        Nothing,
        Uploaded,
        OnCard,
    };

    struct Allocation
    {
        GLenum format = 0;
        QSize size;
    };

    void setupGl();
    // Everything made in the context, which is current and about to go. The frame itself is
    // with the store, so the picture comes back in the next context.
    void releaseGl();
    void forgetGl();
    void closeCard();
    // Takes the newest finished picture from the card when the frame to show is one of its.
    // Until the first is finished, whatever was showing stays up.
    void updateFromCard(bool onCard);
#ifdef ACHERON_HAVE_DMABUF
    // Within withFrame(). A card that will not have the picture is not offered another, and
    // the picture is then in memory, to be uploaded.
    void takeFromCard(const AVFrame *frame, quint64 serial);
#endif
    // Sets the viewport to where the picture belongs: as large as fits, in the middle.
    void placePicture(QSize picture);
    // `program`, bound with its textures and uniforms, over the whole viewport.
    void drawQuad(QOpenGLShaderProgram &program);
    // One RGB texture over the whole viewport.
    void drawTexture(GLuint texture, bool opaque);
    // The overlays change a few times a minute and the picture sixty times a second, so they
    // are painted into an image only when they change, kept as a texture, and laid over each
    // frame with one blended quad.
    void drawOverlays(bool hasPicture);
    // The ring goes on and off with every word, which is too often to paint everything over
    // the picture again for. Two pixels along each edge, as the painter draws it.
    void drawSpeakingRing();
    void uploadPlane(int unit, GLenum format, int textureWidth, int textureHeight, const uint8_t *data);
    void upload(const AVFrame *frame);
    void setConversion(const AVFrame *frame);
    void draw(QSize picture);

    VideoSurface *owner;
    std::shared_ptr<VideoFrameStore> frames;
    std::unique_ptr<QOpenGLShaderProgram> program;
    std::unique_ptr<QOpenGLShaderProgram> plainProgram;
    GLuint overlayTexture = 0;
    QSize overlaySize;
    bool overlaysStale = true;
    bool overlaysHadPicture = false;
    // The planes of a frame uploaded, and then those of one on the card.
    static constexpr size_t FIRST_CARD_TEXTURE = 3;
    std::array<GLuint, 6> textures = {};
    std::array<Allocation, 3> allocations;
#ifdef Q_OS_WIN
    std::shared_ptr<CardPicture> card;
#elif defined(ACHERON_HAVE_DMABUF)
    std::unique_ptr<DmabufGlBridge> card;
#endif
    bool glReady = false;
    GLuint cardTexture = 0;
    Shown shown = Shown::Nothing;
    quint64 preparedSerial = 0;
    bool usable = false;
    bool nv12 = false;
    QVector2D lumaScale;
    QVector2D chromaScale;
    QMatrix3x3 conversion;
    QVector3D offset;
};

} // namespace UI
} // namespace Acheron
