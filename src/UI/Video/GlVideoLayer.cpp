#include "GlVideoLayer.hpp"

#include "GlTextures.hpp"
#include "VideoFrameStore.hpp"
#include "VideoSurface.hpp"

#ifdef Q_OS_WIN
#include "CardWorker.hpp"
#include "D3D11GlBridge.hpp"
#endif
#ifdef ACHERON_HAVE_DMABUF
#include "DmabufGlBridge.hpp"
#endif

#include <QGuiApplication>
#include <QMouseEvent>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QSettings>

extern "C" {
#include <libavutil/frame.h>
}

namespace Acheron {
namespace UI {

namespace {

// a way out, not in the settings window, for drivers that get this wrong
const char *const DIRECT_FRAMES_KEY = "video/direct_frames";

using Core::Media::isFullRange;
using Core::Media::usesBt709;

QOpenGLContext *sharedContext()
{
    static QOpenGLContext *const root = [] {
        auto *context = new QOpenGLContext(qApp);
        context->create();
        return context;
    }();
    return root->isValid() ? root : nullptr;
}

} // namespace

GlVideoLayer::GlVideoLayer(VideoSurface *owner, std::shared_ptr<VideoFrameStore> frames)
    : QOpenGLWindow(sharedContext()), owner(owner), frames(std::move(frames))
{
    // several layers draw from one thread; waiting for vsync in each would stall the others
    QSurfaceFormat format = requestedFormat();
    format.setSwapInterval(0);
    setFormat(format);
}

GlVideoLayer::~GlVideoLayer()
{
    closeCard();
    if (glReady && context() && handle()) {
        makeCurrent();
        releaseGl();
        doneCurrent();
    }
}

QImage GlVideoLayer::snapshot()
{
    if (!isValid() || !handle())
        return {};
    makeCurrent();
    paintGL();
    const QSize area = size() * devicePixelRatio();
    QImage image(area, QImage::Format_RGBA8888);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, area.width(), area.height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    // OpenGL counts rows from the bottom
    image = image.mirrored();
    image.setDevicePixelRatio(devicePixelRatio());
    return image;
}

bool GlVideoLayer::event(QEvent *event)
{
    // the last moment the context can be made current on this window
    if (event->type() == QEvent::PlatformSurface && static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed && glReady) {
        makeCurrent();
        releaseGl();
        doneCurrent();
    }
    return QOpenGLWindow::event(event);
}

void GlVideoLayer::mousePressEvent(QMouseEvent *event)
{
    QCoreApplication::sendEvent(owner, event);
}

void GlVideoLayer::mouseDoubleClickEvent(QMouseEvent *event)
{
    QCoreApplication::sendEvent(owner, event);
}

void GlVideoLayer::mouseReleaseEvent(QMouseEvent *event)
{
    QCoreApplication::sendEvent(owner, event);
    if (event->button() == Qt::RightButton) {
        QContextMenuEvent menu(QContextMenuEvent::Mouse, event->pos(), event->globalPos());
        QCoreApplication::sendEvent(owner, &menu);
    }
}

void GlVideoLayer::keyPressEvent(QKeyEvent *event)
{
    QCoreApplication::sendEvent(owner->window(), event);
}

void GlVideoLayer::focusInEvent(QFocusEvent *event)
{
    QOpenGLWindow::focusInEvent(event);
    // brings the window forward without taking the keyboard from whatever was being typed into
    if (QWindow *top = owner->window()->windowHandle())
        top->requestActivate();
}

void GlVideoLayer::initializeGL()
{
    forgetGl();
}

void GlVideoLayer::setupGl()
{
    initializeOpenGLFunctions();
    program = std::make_unique<QOpenGLShaderProgram>();
    plainProgram = std::make_unique<QOpenGLShaderProgram>();
    glReady = true;

    // GLSL ES 2, which desktop GL also accepts, so it runs through ANGLE and on old drivers
    static const char *const VERTEX = R"(
        attribute vec2 position;
        attribute vec2 texcoord;
        varying vec2 coord;
        void main() {
            coord = texcoord;
            gl_Position = vec4(position, 0.0, 1.0);
        }
    )";
    static const char *const FRAGMENT = R"(
        #ifdef GL_ES
        precision mediump float;
        #endif
        varying vec2 coord;
        uniform sampler2D planeY;
        uniform sampler2D planeU;
        uniform sampler2D planeV;
        uniform float interleaved;
        uniform float pairInGreen;
        uniform vec2 lumaScale;
        uniform vec2 chromaScale;
        uniform mat3 conversion;
        uniform vec3 offset;
        void main() {
            float y = texture2D(planeY, coord * lumaScale).r;
            vec2 at = coord * chromaScale;
            vec2 chroma = interleaved > 0.5 ? texture2D(planeU, at).ra : vec2(texture2D(planeU, at).r, texture2D(planeV, at).r);
            if (pairInGreen > 0.5)
                chroma = texture2D(planeU, at).rg;
            gl_FragColor = vec4(conversion * (vec3(y, chroma) - offset), 1.0);
        }
    )";

    usable = program->addShaderFromSourceCode(QOpenGLShader::Vertex, VERTEX) && program->addShaderFromSourceCode(QOpenGLShader::Fragment, FRAGMENT);
    program->bindAttributeLocation("position", 0);
    program->bindAttributeLocation("texcoord", 1);
    usable = usable && program->link();

    static const char *const PLAIN_FRAGMENT = R"(
        #ifdef GL_ES
        precision mediump float;
        #endif
        varying vec2 coord;
        uniform sampler2D image;
        uniform float opaque;
        void main() {
            vec4 colour = texture2D(image, coord);
            gl_FragColor = opaque > 0.5 ? vec4(colour.rgb, 1.0) : colour;
        }
    )";
    plainProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, VERTEX);
    plainProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, PLAIN_FRAGMENT);
    plainProgram->bindAttributeLocation("position", 0);
    plainProgram->bindAttributeLocation("texcoord", 1);
    plainProgram->link();

    glGenTextures(1, &overlayTexture);
    glBindTexture(GL_TEXTURE_2D, overlayTexture);
    setSampling(this, GL_NEAREST);

    glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
    for (const GLuint texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        setSampling(this, GL_LINEAR);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

#ifdef Q_OS_WIN
    if (usable && plainProgram->isLinked() && QSettings().value(DIRECT_FRAMES_KEY, true).toBool() && D3D11GlBridge::contextSupportsIt()) {
        if (auto worker = CardWorker::sharingWith(context())) {
            card = std::make_shared<CardPicture>(std::move(worker));
            frames->setCard(card);
        }
        // making the worker's context can leave this one not current
        makeCurrent();
    }
#elif defined(ACHERON_HAVE_DMABUF)
    if (usable && QSettings().value(DIRECT_FRAMES_KEY, true).toBool() && DmabufGlBridge::contextSupportsIt()) {
        card = std::make_unique<DmabufGlBridge>();
        frames->setCardUsable(true);
    }
#endif
}

void GlVideoLayer::paintGL()
{
    if (!glReady)
        setupGl();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    QSize picture;
    bool onCard = false;
    frames->withFrame([&](const AVFrame *frame, quint64 serial, QSize displaySize) {
        if (frame->width <= 0 || !usable) {
            shown = Shown::Nothing;
            return;
        }
        picture = displaySize;
#ifdef ACHERON_HAVE_DMABUF
        if (frame->hw_frames_ctx && (shown != Shown::OnCard || serial != preparedSerial))
            takeFromCard(frame, serial);
#endif
        if (frame->hw_frames_ctx) {
            onCard = true;
        } else if (shown != Shown::Uploaded || serial != preparedSerial) {
            upload(frame);
            shown = Shown::Uploaded;
            preparedSerial = serial;
        }
    });
    updateFromCard(onCard);

    if (shown == Shown::Uploaded) {
        draw(picture);
    } else if (shown == Shown::OnCard) {
#ifdef ACHERON_HAVE_DMABUF
        draw(picture);
#else
        placePicture(picture);
        drawTexture(cardTexture, true);
#endif
    }
    drawOverlays(shown != Shown::Nothing);
    if (owner->speaking)
        drawSpeakingRing();
}

void GlVideoLayer::updateFromCard(bool onCard)
{
#ifdef Q_OS_WIN
    if (onCard) {
        cardTexture = card ? card->acquire() : 0;
        if (cardTexture)
            shown = Shown::OnCard;
        else if (shown == Shown::OnCard)
            shown = Shown::Nothing;
    } else if (card && shown == Shown::Nothing) {
        card->discard();
    }
#else
    Q_UNUSED(onCard);
#endif
#ifdef ACHERON_HAVE_DMABUF
    if (card && shown != Shown::OnCard)
        card->release();
#endif
}

#ifdef ACHERON_HAVE_DMABUF
void GlVideoLayer::takeFromCard(const AVFrame *frame, quint64 serial)
{
    const AVFrame *exported = frames->exportedFrame();
    if (!card || !exported || !card->update(exported, textures[FIRST_CARD_TEXTURE], textures[FIRST_CARD_TEXTURE + 1])) {
        closeCard();
        frames->readBack();
        return;
    }
    nv12 = true;
    // the surface is the coded size, rounded up to whole blocks
    lumaScale = QVector2D((static_cast<float>(frame->width) - 0.5f) / static_cast<float>(exported->width), (static_cast<float>(frame->height) - 0.5f) / static_cast<float>(exported->height));
    chromaScale = QVector2D((static_cast<float>((frame->width + 1) / 2) - 0.5f) / static_cast<float>((exported->width + 1) / 2), (static_cast<float>((frame->height + 1) / 2) - 0.5f) / static_cast<float>((exported->height + 1) / 2));
    setConversion(frame);
    shown = Shown::OnCard;
    preparedSerial = serial;
}
#endif

void GlVideoLayer::releaseGl()
{
    closeCard();
    glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    glDeleteTextures(1, &overlayTexture);
    forgetGl();
}

void GlVideoLayer::forgetGl()
{
    closeCard();
    textures = {};
    overlayTexture = 0;
    allocations = {};
    overlaySize = {};
    overlaysStale = true;
    program.reset();
    plainProgram.reset();
    usable = false;
    glReady = false;
    shown = Shown::Nothing;
}

void GlVideoLayer::closeCard()
{
#ifdef Q_OS_WIN
    if (card) {
        frames->setCard(nullptr);
        card->close();
        card.reset();
    }
#elif defined(ACHERON_HAVE_DMABUF)
    if (card) {
        frames->setCardUsable(false);
        card.reset();
    }
#endif
    cardTexture = 0;
    if (shown == Shown::OnCard)
        shown = Shown::Nothing;
}

void GlVideoLayer::placePicture(QSize picture)
{
    const QSize area = size() * devicePixelRatio();
    const QSize where = fitted(picture, area);
    glViewport((area.width() - where.width()) / 2, (area.height() - where.height()) / 2, where.width(), where.height());
}

void GlVideoLayer::drawQuad(QOpenGLShaderProgram &shader)
{
    static const GLfloat POSITIONS[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
    static const GLfloat TEXCOORDS[] = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    shader.enableAttributeArray(0);
    shader.enableAttributeArray(1);
    shader.setAttributeArray(0, GL_FLOAT, POSITIONS, 2);
    shader.setAttributeArray(1, GL_FLOAT, TEXCOORDS, 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    shader.disableAttributeArray(0);
    shader.disableAttributeArray(1);
}

void GlVideoLayer::drawTexture(GLuint texture, bool opaque)
{
    plainProgram->bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    plainProgram->setUniformValue("image", 0);
    plainProgram->setUniformValue("opaque", opaque ? 1.0f : 0.0f);
    drawQuad(*plainProgram);
    glBindTexture(GL_TEXTURE_2D, 0);
    plainProgram->release();
}

void GlVideoLayer::drawOverlays(bool hasPicture)
{
    const qreal ratio = devicePixelRatio();
    const QSize area = size() * ratio;
    if (area.isEmpty() || !plainProgram || !plainProgram->isLinked())
        return;
    if (!owner->hasOverlaysBesideTheRing(hasPicture))
        return;

    if (overlaysStale || hasPicture != overlaysHadPicture || area != overlaySize) {
        QImage image(area, QImage::Format_RGBA8888_Premultiplied);
        image.setDevicePixelRatio(ratio);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            owner->paintOverlays(painter, hasPicture, false);
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, overlayTexture);
        if (area != overlaySize)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, area.width(), area.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
        else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, area.width(), area.height(), GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());

        overlaysStale = false;
        overlaysHadPicture = hasPicture;
        overlaySize = area;
    }

    glViewport(0, 0, area.width(), area.height());
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    drawTexture(overlayTexture, false);
    glDisable(GL_BLEND);
}

void GlVideoLayer::drawSpeakingRing()
{
    const QSize area = size() * devicePixelRatio();
    const int thickness = qMax(1, qRound(2 * devicePixelRatio()));
    glEnable(GL_SCISSOR_TEST);
    glClearColor(static_cast<GLfloat>(SPEAKING_RING.redF()), static_cast<GLfloat>(SPEAKING_RING.greenF()), static_cast<GLfloat>(SPEAKING_RING.blueF()), 1.0f);
    for (const QRect &edge : { QRect(0, 0, area.width(), thickness), QRect(0, area.height() - thickness, area.width(), thickness), QRect(0, 0, thickness, area.height()), QRect(area.width() - thickness, 0, thickness, area.height()) }) {
        glScissor(edge.x(), edge.y(), edge.width(), edge.height());
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
}

void GlVideoLayer::uploadPlane(int unit, GLenum format, int textureWidth, int textureHeight, const uint8_t *data)
{
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    glBindTexture(GL_TEXTURE_2D, textures[static_cast<size_t>(unit)]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    Allocation &allocation = allocations[static_cast<size_t>(unit)];
    if (allocation.format != format || allocation.size != QSize(textureWidth, textureHeight)) {
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format), textureWidth, textureHeight, 0, format, GL_UNSIGNED_BYTE, data);
        allocation = { format, QSize(textureWidth, textureHeight) };
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, textureWidth, textureHeight, format, GL_UNSIGNED_BYTE, data);
    }
}

void GlVideoLayer::upload(const AVFrame *frame)
{
    const int chromaWidth = (frame->width + 1) / 2;
    const int chromaHeight = (frame->height + 1) / 2;
    nv12 = frame->format == AV_PIX_FMT_NV12;

    // whole rows go up, padding included. half a texel is held back so filtering doesnt reach it
    uploadPlane(0, GL_LUMINANCE, frame->linesize[0], frame->height, frame->data[0]);
    lumaScale = QVector2D((static_cast<float>(frame->width) - 0.5f) / static_cast<float>(frame->linesize[0]), 1.0f);

    if (nv12) {
        const int pairs = frame->linesize[1] / 2;
        uploadPlane(1, GL_LUMINANCE_ALPHA, pairs, chromaHeight, frame->data[1]);
        chromaScale = QVector2D((static_cast<float>(chromaWidth) - 0.5f) / static_cast<float>(pairs), 1.0f);
    } else {
        uploadPlane(1, GL_LUMINANCE, frame->linesize[1], chromaHeight, frame->data[1]);
        uploadPlane(2, GL_LUMINANCE, frame->linesize[2], chromaHeight, frame->data[2]);
        chromaScale = QVector2D((static_cast<float>(chromaWidth) - 0.5f) / static_cast<float>(frame->linesize[1]), 1.0f);
    }
    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    setConversion(frame);
}

void GlVideoLayer::setConversion(const AVFrame *frame)
{
    const bool bt709 = usesBt709(frame);
    const float kr = bt709 ? 0.2126f : 0.299f;
    const float kb = bt709 ? 0.0722f : 0.114f;
    const float kg = 1.0f - kr - kb;
    const bool full = isFullRange(frame);
    const float lumaGain = full ? 1.0f : 255.0f / 219.0f;
    const float chromaGain = full ? 1.0f : 255.0f / 224.0f;
    const float values[9] = {
        lumaGain, 0.0f, (2.0f - 2.0f * kr) * chromaGain,
        lumaGain, -(2.0f * kb * (1.0f - kb) / kg) * chromaGain, -(2.0f * kr * (1.0f - kr) / kg) * chromaGain,
        lumaGain, (2.0f - 2.0f * kb) * chromaGain, 0.0f
    };
    conversion = QMatrix3x3(values);
    offset = QVector3D(full ? 0.0f : 16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f);
}

void GlVideoLayer::draw(QSize picture)
{
    placePicture(picture);

    program->bind();
    const size_t first = shown == Shown::OnCard ? FIRST_CARD_TEXTURE : 0;
    for (int unit = 0; unit < PICTURE_PLANES; unit++) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glBindTexture(GL_TEXTURE_2D, textures[first + static_cast<size_t>(unit)]);
    }
    program->setUniformValue("planeY", 0);
    program->setUniformValue("planeU", 1);
    program->setUniformValue("planeV", 2);
    program->setUniformValue("interleaved", nv12 ? 1.0f : 0.0f);
    program->setUniformValue("pairInGreen", shown == Shown::OnCard ? 1.0f : 0.0f);
    program->setUniformValue("lumaScale", lumaScale);
    program->setUniformValue("chromaScale", chromaScale);
    program->setUniformValue("conversion", conversion);
    program->setUniformValue("offset", offset);
    drawQuad(*program);

    for (int unit = 2; unit >= 0; unit--) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    program->release();
}

} // namespace UI
} // namespace Acheron
