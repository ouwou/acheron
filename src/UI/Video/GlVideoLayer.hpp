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

// a window inside the surface's window, not a widget: an OpenGL widget makes Qt recreate the whole
// window it is in, visibly, and composite all of it through OpenGL on every frame
class GlVideoLayer : public QOpenGLWindow, protected QOpenGLFunctions
{
public:
    GlVideoLayer(VideoSurface *owner, std::shared_ptr<VideoFrameStore> frames);
    ~GlVideoLayer() override;

    void invalidateOverlays() { overlaysStale = true; }
    [[nodiscard]] bool drawsFromCard() const { return shown == Shown::OnCard; }
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
    void releaseGl();
    void forgetGl();
    void closeCard();
    void updateFromCard(bool onCard);
#ifdef ACHERON_HAVE_DMABUF
    void takeFromCard(const AVFrame *frame, quint64 serial);
#endif
    void placePicture(QSize picture);
    void drawQuad(QOpenGLShaderProgram &program);
    void drawTexture(GLuint texture, bool opaque);
    void drawOverlays(bool hasPicture);
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
    static constexpr int PICTURE_PLANES = 3;
    static constexpr size_t FIRST_CARD_TEXTURE = PICTURE_PLANES;
    std::array<GLuint, 2 * PICTURE_PLANES> textures = {};
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
