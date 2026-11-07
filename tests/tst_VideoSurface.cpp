#include "Core/Media/HardwareDevices.hpp"
#include "Core/Media/VideoDecoder.hpp"
#include "HardwareTests.hpp"
#include "TestPictures.hpp"
#include "UI/Video/VideoSurface.hpp"

#include <QDir>
#include <QEvent>
#include <QGuiApplication>
#include <QImage>
#include <QLibrary>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QScreen>
#include <QSignalSpy>
#include <QWindow>
#include <QTest>

#include <algorithm>

#ifdef Q_OS_WIN
// first: hwcontext_d3d11va.h would include it inside extern "C"
#include <d3d11.h>
#undef small
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#if defined(Q_OS_WIN) && __has_include(<libavutil/hwcontext_d3d11va.h>)
#include <libavutil/hwcontext_d3d11va.h>
#define HAVE_D3D11VA 1
#define CARD_FORMAT AV_PIX_FMT_D3D11
#elif defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
#include <libavutil/hwcontext_drm.h>
#define HAVE_VAAPI 1
#define CARD_FORMAT AV_PIX_FMT_VAAPI
#endif
}

using Acheron::Core::Media::BufferPointer;
using Acheron::Core::Media::FramePointer;
using Acheron::UI::VideoSurface;
using namespace TestPictures;

namespace {

const char CANNOT_DRAW_FROM_CARD[] = "This surface cannot draw the card's picture where it is";
const char NO_DEVICE_TO_DRAW_FROM[] = "No video device whose pictures this surface can draw";
constexpr uint8_t GREY_LUMA = 126;

[[nodiscard]] bool shown(QWidget &window, const QSize &size = QSize(320, 180))
{
    window.resize(size);
    window.show();
    return QTest::qWaitForWindowExposed(&window);
}

FramePointer withGreyRightHalf(FramePointer frame)
{
    for (int row = 0; row < frame->height; row++)
        memset(frame->data[0] + row * frame->linesize[0] + frame->width / 2, GREY_LUMA, static_cast<size_t>(frame->width - frame->width / 2));
    return frame;
}

void present(VideoSurface &surface, FramePointer frame)
{
    surface.sink()->present(frame.get());
    frame.reset();
    QCoreApplication::processEvents();
}

QImage shot(VideoSurface &surface)
{
    QImage image;
    if (!QTest::qWaitFor([&] { image = surface.snapshot(); return !image.isNull(); }, 2000))
        return {};
    return image.convertToFormat(QImage::Format_RGB32);
}

void drawOnce(VideoSurface &surface)
{
    present(surface, solidFrame(BLACK, 64, 36));
    shot(surface);
}

QList<QWindow *> layersOf(const VideoSurface &surface)
{
    QList<QWindow *> layers;
    const QWindowList windows = QGuiApplication::allWindows();
    for (QWindow *window : windows) {
        if (surface.windowHandle() && window->parent() == surface.windowHandle())
            layers.append(window);
    }
    return layers;
}

bool isNear(QRgb pixel, int red, int green, int blue)
{
    constexpr int TOLERANCE = 12;
    return qAbs(qRed(pixel) - red) <= TOLERANCE && qAbs(qGreen(pixel) - green) <= TOLERANCE && qAbs(qBlue(pixel) - blue) <= TOLERANCE;
}

bool isNear(QRgb pixel, const QColor &colour)
{
    return isNear(pixel, colour.red(), colour.green(), colour.blue());
}

#ifdef CARD_FORMAT
// known only once a first picture has made the surface's layer
[[nodiscard]] bool takesCardPictures(VideoSurface &surface)
{
    drawOnce(surface);
    return surface.sink()->acceptsHardwareFrames(CARD_FORMAT);
}

FramePointer solidNv12Frame(YuvSamples colour, int width, int height)
{
    FramePointer memory(av_frame_alloc());
    if (!memory || Acheron::Core::Media::prepareWritable(memory.get(), AV_PIX_FMT_NV12, QSize(width, height)) < 0)
        return nullptr;
    for (int row = 0; row < height; row++)
        memset(memory->data[0] + row * memory->linesize[0], colour.y, static_cast<size_t>(width));
    for (int row = 0; row < height / 2; row++) {
        uint8_t *pair = memory->data[1] + row * memory->linesize[1];
        for (int column = 0; column < width / 2; column++) {
            *pair++ = colour.u;
            *pair++ = colour.v;
        }
    }
    return memory;
}

BufferPointer cardPool(const char *device, int width, int height, int surfaces = 0, AVPixelFormat layout = AV_PIX_FMT_NV12)
{
#ifdef HAVE_D3D11VA
    constexpr AVHWDeviceType TYPE = AV_HWDEVICE_TYPE_D3D11VA;
#else
    constexpr AVHWDeviceType TYPE = AV_HWDEVICE_TYPE_VAAPI;
#endif
    AVBufferRef *created = nullptr;
    if (!Acheron::Core::Media::hardwareDeviceCanBeAskedFor(TYPE) || av_hwdevice_ctx_create(&created, TYPE, device, nullptr, 0) < 0)
        return nullptr;
    const BufferPointer card(created);
    BufferPointer pool(av_hwframe_ctx_alloc(card.get()));
    if (!pool)
        return nullptr;
    auto *frames = reinterpret_cast<AVHWFramesContext *>(pool->data);
    frames->format = CARD_FORMAT;
    frames->sw_format = layout;
    frames->width = width;
    frames->height = height;
    frames->initial_pool_size = surfaces;
#ifdef HAVE_D3D11VA
    static_cast<AVD3D11VAFramesContext *>(frames->hwctx)->BindFlags = D3D11_BIND_DECODER;
#endif
    if (av_hwframe_ctx_init(pool.get()) < 0)
        return nullptr;
    return pool;
}

FramePointer onCard(AVBufferRef *pool, const AVFrame *memory)
{
    FramePointer frame(av_frame_alloc());
    if (!frame || !memory || av_hwframe_get_buffer(pool, frame.get(), 0) < 0 || av_hwframe_transfer_data(frame.get(), memory, 0) < 0)
        return nullptr;
    return frame;
}

FramePointer solidFrameFrom(AVBufferRef *pool, YuvSamples colour)
{
    const auto *frames = reinterpret_cast<const AVHWFramesContext *>(pool->data);
    return onCard(pool, solidNv12Frame(colour, frames->width, frames->height).get());
}
#endif

#ifdef HAVE_D3D11VA
FramePointer onCard(const AVFrame *memory)
{
    const BufferPointer pool = cardPool(nullptr, memory->width, memory->height, 2);
    return pool ? onCard(pool.get(), memory) : nullptr;
}

FramePointer solidFrameOnCard(YuvSamples colour, int width, int height)
{
    return onCard(solidNv12Frame(colour, width, height).get());
}
#endif

#ifdef HAVE_VAAPI
// drm_fourcc.h
constexpr quint64 MODIFIER_LINEAR = 0;
constexpr quint64 MODIFIER_INVALID = 0x00ffffffffffffffULL;

FramePointer solidP010Frame(YuvSamples colour, int width, int height)
{
    FramePointer memory(av_frame_alloc());
    if (!memory || Acheron::Core::Media::prepareWritable(memory.get(), AV_PIX_FMT_P010, QSize(width, height)) < 0)
        return nullptr;
    for (int row = 0; row < height; row++) {
        auto *line = reinterpret_cast<uint16_t *>(memory->data[0] + row * memory->linesize[0]);
        std::fill(line, line + width, static_cast<uint16_t>(colour.y << 8));
    }
    for (int row = 0; row < height / 2; row++) {
        auto *pair = reinterpret_cast<uint16_t *>(memory->data[1] + row * memory->linesize[1]);
        for (int column = 0; column < width / 2; column++) {
            *pair++ = static_cast<uint16_t>(colour.u << 8);
            *pair++ = static_cast<uint16_t>(colour.v << 8);
        }
    }
    return memory;
}

FramePointer rampNv12Frame(QSize picture, QSize coded)
{
    FramePointer memory = solidNv12Frame(WHITE, coded.width(), coded.height());
    for (int row = 0; row < picture.height(); row++) {
        for (int column = 0; column < picture.width(); column++)
            memory->data[0][row * memory->linesize[0] + column] = static_cast<uint8_t>(16 + 219 * column / picture.width());
    }
    for (int row = 0; row < picture.height() / 2; row++) {
        for (int column = 0; column < picture.width() / 2; column++)
            memory->data[1][row * memory->linesize[1] + 2 * column] = static_cast<uint8_t>(16 + 224 * row / (picture.height() / 2));
    }
    return memory;
}

FramePointer solidFrameOnNode(const QByteArray &node, YuvSamples colour, int width, int height)
{
    const BufferPointer pool = cardPool(node.constData(), width, height);
    return pool ? solidFrameFrom(pool.get(), colour) : nullptr;
}

QList<QByteArray> renderNodes()
{
    QList<QByteArray> nodes;
    const QStringList names = QDir(QStringLiteral("/dev/dri")).entryList({ QStringLiteral("renderD*") }, QDir::System);
    for (const QString &name : names)
        nodes.append("/dev/dri/" + name.toLatin1());
    return nodes;
}

struct EglHere
{
    bool present = false;
    bool takesModifiers = false;
    QByteArray renderNode;
};

const EglHere &eglHere()
{
    static const EglHere here = [] {
        // EGL/egl.h and EGL/eglext.h
        constexpr int EXTENSIONS = 0x3055;
        constexpr int DEVICE = 0x322C;
        constexpr int DRM_RENDER_NODE_FILE = 0x3377;

        EglHere found;
        QOffscreenSurface offscreen;
        offscreen.create();
        QOpenGLContext context;
        const auto currentDisplay = reinterpret_cast<void *(*)()>(QLibrary::resolve(QStringLiteral("EGL"), 1, "eglGetCurrentDisplay"));
        void *display = currentDisplay && context.create() && context.makeCurrent(&offscreen) ? currentDisplay() : nullptr;
        if (!display)
            return found;
        found.present = true;
        const auto queryString = reinterpret_cast<const char *(*)(void *, int)>(context.getProcAddress("eglQueryString"));
        found.takesModifiers = strstr(queryString(display, EXTENSIONS), "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;
        const auto displayAttribute = reinterpret_cast<unsigned int (*)(void *, int, qintptr *)>(context.getProcAddress("eglQueryDisplayAttribEXT"));
        const auto deviceString = reinterpret_cast<const char *(*)(void *, int)>(context.getProcAddress("eglQueryDeviceStringEXT"));
        qintptr device = 0;
        if (displayAttribute && deviceString && displayAttribute(display, DEVICE, &device) && device)
            found.renderNode = deviceString(reinterpret_cast<void *>(device), DRM_RENDER_NODE_FILE);
        return found;
    }();
    return here;
}

quint64 modifierOf(const AVFrame *frame)
{
    const FramePointer exported(av_frame_alloc());
    exported->format = AV_PIX_FMT_DRM_PRIME;
    if (av_hwframe_map(exported.get(), frame, AV_HWFRAME_MAP_READ) < 0)
        return MODIFIER_INVALID;
    return reinterpret_cast<const AVDRMFrameDescriptor *>(exported->data[0])->objects[0].format_modifier;
}

QByteArray drawingNode()
{
    static const QByteArray found = [] {
        const QByteArray node = eglHere().renderNode;
        VideoSurface surface;
        if (node.isEmpty() || !shown(surface, QSize(160, 90)))
            return QByteArray();
        FramePointer frame = solidFrameOnNode(node, WHITE, 640, 360);
        if (!frame)
            return QByteArray();
        shot(surface);
        present(surface, std::move(frame));
        shot(surface);
        return surface.drawsFromCard() ? node : QByteArray();
    }();
    return found;
}

FramePointer solidFrameOnCard(YuvSamples colour, int width, int height)
{
    return drawingNode().isEmpty() ? nullptr : solidFrameOnNode(drawingNode(), colour, width, height);
}
#endif

} // namespace

class TestVideoSurface : public QObject
{
    Q_OBJECT
    void turnedPictureRows();
    void verifyTurnedPicture(const QImage &image);

private slots:
    void initTestCase();
    void frameFillsAMatchingSurface();
    void narrowerFrameIsCentredBetweenBars();
    void aTurnedPictureIsDrawnTheRightWayUp_data() { turnedPictureRows(); }
    void aTurnedPictureIsDrawnTheRightWayUp();
    void aTurnedPictureOnTheCardIsDrawnTheRightWayUp_data() { turnedPictureRows(); }
    void aTurnedPictureOnTheCardIsDrawnTheRightWayUp();
    void coloursFollowTheVideoStandard();
    void memoryEglCannotBeToldTheOrderOfIsReadBack();
    void frameOnTheGraphicsCardIsDrawnFromThere();
    void manySurfacesDrawFromTheGraphicsCard();
    void pictureFromTheCardIsThePictureInMemory();
    void tenBitPictureFromTheCardIsReadBack();
    void cardSurfaceIsKeptFromTheDecoderWhileItIsDrawn();
    void pictureSurvivesAMoveToAnotherWindow();
    void clickAndDoubleClickAreReported();
    void geometryIsReported();
    void resizeRedrawsTheLastFrame();
    void messagesDoNotDisturbThePicture();
    void avatarStandsInUntilThereIsAPicture();
    void speakingRingIsDrawn();
    void aSurfaceHasItsLayerOnlyWhileItHasAPicture();
    void aSurfaceLongOffTheScreenGivesUpItsLayerAndKeepsItsPicture();
    void statisticsAreLeftOffASmallTile();
    // last: a card found not to hand its pictures over isnt asked again in this process
    void pictureFromAnotherCardIsReadBack();
};

void TestVideoSurface::initTestCase()
{
    QOffscreenSurface offscreen;
    offscreen.create();
    QOpenGLContext context;
    if (!context.create() || !context.makeCurrent(&offscreen))
        return;
    qInfo().noquote() << QStringLiteral("OpenGL draws with %1").arg(QString::fromLatin1(reinterpret_cast<const char *>(context.functions()->glGetString(GL_RENDERER))));
}

void TestVideoSurface::frameFillsAMatchingSurface()
{
    VideoSurface surface;
    QVERIFY(shown(surface));

    present(surface, solidFrame(WHITE, 640, 360));

    const QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(0, 0), 255, 255, 255));
    QVERIFY(isNear(image.pixel(image.width() - 1, image.height() - 1), 255, 255, 255));
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));
}

void TestVideoSurface::narrowerFrameIsCentredBetweenBars()
{
    VideoSurface surface;
    QVERIFY(shown(surface));

    // 4:3 in a 16:9 surface: 240 wide, with a 40 pixel bar on each side
    present(surface, solidFrame(WHITE, 640, 480));

    const QImage image = shot(surface);
    const qreal ratio = image.devicePixelRatio();
    const int middle = image.height() / 2;
    QVERIFY(isNear(image.pixel(qRound(20 * ratio), middle), 0, 0, 0));
    QVERIFY(isNear(image.pixel(qRound(300 * ratio), middle), 0, 0, 0));
    QVERIFY(isNear(image.pixel(qRound(45 * ratio), middle), 255, 255, 255));
    QVERIFY(isNear(image.pixel(qRound(275 * ratio), middle), 255, 255, 255));
    QVERIFY(isNear(image.pixel(image.width() / 2, 0), 255, 255, 255));
}

void TestVideoSurface::turnedPictureRows()
{
    QTest::addColumn<int>("quarterTurns");
    QTest::addColumn<QPointF>("whiteAt");
    QTest::addColumn<QPointF>("greyAt");
    QTest::addColumn<bool>("sideways");
    QTest::newRow("upright") << 0 << QPointF(0.25, 0.5) << QPointF(0.75, 0.5) << false;
    QTest::newRow("90 degrees") << 1 << QPointF(0.5, 0.25) << QPointF(0.5, 0.75) << true;
    QTest::newRow("180 degrees") << 2 << QPointF(0.75, 0.5) << QPointF(0.25, 0.5) << false;
    QTest::newRow("270 degrees") << 3 << QPointF(0.5, 0.75) << QPointF(0.5, 0.25) << true;
}

void TestVideoSurface::verifyTurnedPicture(const QImage &image)
{
    QFETCH(QPointF, whiteAt);
    QFETCH(QPointF, greyAt);
    QFETCH(bool, sideways);

    const auto at = [&image](QPointF place) { return image.pixel(qRound(place.x() * (image.width() - 1)), qRound(place.y() * (image.height() - 1))); };
    QVERIFY(isNear(at(whiteAt), 255, 255, 255));
    QVERIFY(isNear(at(greyAt), 128, 128, 128));
    // twice as wide as it is high, so its bars lie across the way it is turned
    QVERIFY(isNear(at(sideways ? QPointF(0.1, 0.5) : QPointF(0.5, 0.1)), 0, 0, 0));
    QVERIFY(isNear(at(sideways ? QPointF(0.9, 0.5) : QPointF(0.5, 0.9)), 0, 0, 0));
    QVERIFY(!isNear(at(sideways ? QPointF(0.5, 0.1) : QPointF(0.1, 0.5)), 0, 0, 0));
}

void TestVideoSurface::aTurnedPictureIsDrawnTheRightWayUp()
{
    QFETCH(int, quarterTurns);

    VideoSurface surface;
    QVERIFY(shown(surface, QSize(320, 320)));

    FramePointer frame = withGreyRightHalf(solidFrame(WHITE, 640, 320));
    Acheron::Core::Media::setQuarterTurns(frame.get(), quarterTurns);
    present(surface, std::move(frame));
    verifyTurnedPicture(shot(surface));
}

void TestVideoSurface::aTurnedPictureOnTheCardIsDrawnTheRightWayUp()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#ifdef HAVE_D3D11VA
    QFETCH(int, quarterTurns);

    VideoSurface surface;
    QVERIFY(shown(surface, QSize(320, 320)));
    if (!takesCardPictures(surface))
        QSKIP(CANNOT_DRAW_FROM_CARD);

    FramePointer frame = onCard(withGreyRightHalf(solidNv12Frame(WHITE, 640, 320)).get());
    if (!frame)
        QSKIP(NO_DEVICE_TO_DRAW_FROM);
    Acheron::Core::Media::setQuarterTurns(frame.get(), quarterTurns);
    present(surface, std::move(frame));
    QTRY_VERIFY(surface.drawsFromCard());
    verifyTurnedPicture(shot(surface));
#else
    QSKIP("Direct3D 11 only");
#endif
}

void TestVideoSurface::coloursFollowTheVideoStandard()
{
    VideoSurface surface;
    QVERIFY(shown(surface));

    // untagged, so read by their height
    present(surface, solidFrame(RED_BT709, 1280, 720));
    QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));

    present(surface, solidFrame(RED_BT601, 640, 360));
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));
}

void TestVideoSurface::memoryEglCannotBeToldTheOrderOfIsReadBack()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#ifdef HAVE_VAAPI
    if (!eglHere().present || eglHere().takesModifiers)
        QSKIP("Only for an EGL that cannot be told how a card orders its memory");
    FramePointer frame = eglHere().renderNode.isEmpty() ? nullptr : solidFrameOnNode(eglHere().renderNode, RED_BT709, 1280, 720);
    if (!frame)
        QSKIP("No video device on the card that draws");
    const quint64 modifier = modifierOf(frame.get());

    VideoSurface surface;
    QVERIFY(shown(surface));
    const bool offered = takesCardPictures(surface);
    present(surface, std::move(frame));
    const QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));
    QCOMPARE(surface.drawsFromCard(), offered && (modifier == MODIFIER_LINEAR || modifier == MODIFIER_INVALID));
#else
    QSKIP("VAAPI only");
#endif
}

void TestVideoSurface::frameOnTheGraphicsCardIsDrawnFromThere()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#ifdef CARD_FORMAT
    VideoSurface surface;
    QVERIFY(shown(surface));
    if (!takesCardPictures(surface))
        QSKIP(CANNOT_DRAW_FROM_CARD);
    QVERIFY(!surface.sink()->acceptsHardwareFrames(AV_PIX_FMT_DXVA2_VLD));

    FramePointer frame = solidFrameOnCard(RED_BT709, 1280, 720);
    if (!frame)
        QSKIP(NO_DEVICE_TO_DRAW_FROM);
    present(surface, std::move(frame));
    QTRY_VERIFY(surface.drawsFromCard());
    QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(0, 0), 255, 0, 0));
    QVERIFY(isNear(image.pixel(image.width() - 1, image.height() - 1), 255, 0, 0));
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));

    frame = solidFrameOnCard(RED_BT601, 640, 480);
    QVERIFY(frame);
    const qreal ratio = image.devicePixelRatio();
    const int middle = image.height() / 2;
    present(surface, std::move(frame));
    QTRY_VERIFY(isNear(shot(surface).pixel(qRound(20 * ratio), middle), 0, 0, 0));
    image = shot(surface);
    QVERIFY(surface.drawsFromCard());
    QVERIFY(isNear(image.pixel(qRound(45 * ratio), middle), 255, 0, 0));
    QVERIFY(isNear(image.pixel(qRound(275 * ratio), middle), 255, 0, 0));
    QVERIFY(isNear(image.pixel(qRound(300 * ratio), middle), 0, 0, 0));
    QVERIFY(isNear(image.pixel(image.width() / 2, 0), 255, 0, 0));

    surface.setSpeaking(true);
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, middle), 255, 0, 0));
    QVERIFY(!isNear(image.pixel(1, middle), 0, 0, 0));
    surface.setSpeaking(false);

    present(surface, solidFrame(WHITE, 640, 360));
    image = shot(surface);
    QVERIFY(!surface.drawsFromCard());
    QVERIFY(isNear(image.pixel(image.width() / 2, middle), 255, 255, 255));

    surface.clear();
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, middle), 0, 0, 0));
#else
    QSKIP("Direct3D 11 and VAAPI only");
#endif
}

void TestVideoSurface::manySurfacesDrawFromTheGraphicsCard()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#ifdef CARD_FORMAT
    constexpr int COUNT = 5;
    QWidget window;
    QList<VideoSurface *> surfaces;
    for (int i = 0; i < COUNT; i++) {
        auto *surface = new VideoSurface(&window);
        surface->setGeometry(i * 160, 0, 160, 90);
        surfaces.append(surface);
    }
    QVERIFY(shown(window, QSize(COUNT * 160, 90)));
    for (VideoSurface *surface : surfaces)
        drawOnce(*surface);
    if (!surfaces.first()->sink()->acceptsHardwareFrames(CARD_FORMAT))
        QSKIP(CANNOT_DRAW_FROM_CARD);

    for (int i = 0; i < COUNT; i++) {
        FramePointer frame = solidFrameOnCard(i % 2 == 0 ? WHITE : RED_BT601, 640, 360);
        if (!frame)
            QSKIP(NO_DEVICE_TO_DRAW_FROM);
        present(*surfaces[i], std::move(frame));
    }
    for (VideoSurface *surface : surfaces)
        QTRY_VERIFY(surface->drawsFromCard());

    for (int i = 0; i < COUNT; i++) {
        const QImage image = shot(*surfaces[i]);
        const QRgb pixel = image.pixel(image.width() / 2, image.height() / 2);
        QVERIFY2(isNear(pixel, 255, i % 2 == 0 ? 255 : 0, i % 2 == 0 ? 255 : 0), qPrintable(QStringLiteral("surface %1").arg(i)));
    }

    surfaces[1]->hide();
    present(*surfaces[1], solidFrameOnCard(WHITE, 640, 360));
    surfaces[1]->show();
    QTRY_VERIFY(isNear(shot(*surfaces[1]).pixel(40, 40), 255, 255, 255));
#else
    QSKIP("Direct3D 11 and VAAPI only");
#endif
}

void TestVideoSurface::pictureFromTheCardIsThePictureInMemory()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#ifdef HAVE_VAAPI
    VideoSurface surface;
    QVERIFY(shown(surface));
    if (!takesCardPictures(surface))
        QSKIP(CANNOT_DRAW_FROM_CARD);
    if (drawingNode().isEmpty())
        QSKIP(NO_DEVICE_TO_DRAW_FROM);

    const QSize picture(500, 282);
    const QSize coded(512, 288);
    FramePointer memory = rampNv12Frame(picture, coded);
    const BufferPointer pool = cardPool(drawingNode().constData(), coded.width(), coded.height());
    QVERIFY(pool);
    FramePointer card = onCard(pool.get(), memory.get());
    QVERIFY(card);

    memory->width = picture.width();
    memory->height = picture.height();
    present(surface, std::move(memory));
    const QImage fromMemory = shot(surface);
    QVERIFY(!surface.drawsFromCard());
    QVERIFY(qBlue(fromMemory.pixel(160, 170)) > qBlue(fromMemory.pixel(160, 10)) + 100);
    QVERIFY(qGreen(fromMemory.pixel(310, 90)) > qGreen(fromMemory.pixel(10, 90)) + 100);

    card->width = picture.width();
    card->height = picture.height();
    present(surface, std::move(card));
    const QImage fromCard = shot(surface);
    QVERIFY(surface.drawsFromCard());
    QCOMPARE(fromCard.size(), fromMemory.size());
    for (int y = 0; y < fromCard.height(); y++) {
        for (int x = 0; x < fromCard.width(); x++) {
            const QRgb expected = fromMemory.pixel(x, y);
            QVERIFY2(isNear(fromCard.pixel(x, y), qRed(expected), qGreen(expected), qBlue(expected)), qPrintable(QStringLiteral("at %1, %2").arg(x).arg(y)));
        }
    }
#else
    QSKIP("VAAPI only");
#endif
}

void TestVideoSurface::tenBitPictureFromTheCardIsReadBack()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#ifdef HAVE_VAAPI
    VideoSurface surface;
    QVERIFY(shown(surface));
    if (!takesCardPictures(surface))
        QSKIP(CANNOT_DRAW_FROM_CARD);
    if (drawingNode().isEmpty())
        QSKIP(NO_DEVICE_TO_DRAW_FROM);
    const BufferPointer pool = cardPool(drawingNode().constData(), 1280, 720, 0, AV_PIX_FMT_P010);
    if (!pool)
        QSKIP("No ten-bit surfaces on this card");

    FramePointer frame = onCard(pool.get(), solidP010Frame(RED_BT709, 1280, 720).get());
    QVERIFY(frame);
    present(surface, std::move(frame));
    const QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));
    QVERIFY(!surface.drawsFromCard());
    QVERIFY(!surface.sink()->acceptsHardwareFrames(CARD_FORMAT));

    VideoSurface other;
    QVERIFY(shown(other));
    QVERIFY(takesCardPictures(other));
#else
    QSKIP("VAAPI only");
#endif
}

void TestVideoSurface::cardSurfaceIsKeptFromTheDecoderWhileItIsDrawn()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#ifdef HAVE_VAAPI
    VideoSurface surface;
    QVERIFY(shown(surface));
    if (!takesCardPictures(surface))
        QSKIP(CANNOT_DRAW_FROM_CARD);
    if (drawingNode().isEmpty())
        QSKIP(NO_DEVICE_TO_DRAW_FROM);

    const BufferPointer pool = cardPool(drawingNode().constData(), 640, 360, 2);
    QVERIFY(pool);
    present(surface, solidFrameFrom(pool.get(), RED_BT601));
    QImage image = shot(surface);
    QVERIFY(surface.drawsFromCard());
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));

    FramePointer white = solidFrameFrom(pool.get(), WHITE);
    QVERIFY(white);
    surface.sink()->present(white.get());
    white.reset();
    const FramePointer spare(av_frame_alloc());
    QVERIFY(av_hwframe_get_buffer(pool.get(), spare.get(), 0) < 0);

    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));
    QCOMPARE(av_hwframe_get_buffer(pool.get(), spare.get(), 0), 0);

    surface.clear();
    const FramePointer other(av_frame_alloc());
    QVERIFY(av_hwframe_get_buffer(pool.get(), other.get(), 0) < 0);
    shot(surface);
    QCOMPARE(av_hwframe_get_buffer(pool.get(), other.get(), 0), 0);
#else
    QSKIP("VAAPI only");
#endif
}

void TestVideoSurface::pictureSurvivesAMoveToAnotherWindow()
{
    QWidget first;
    auto *surface = new VideoSurface(&first);
    surface->setGeometry(0, 0, 320, 180);
    QVERIFY(shown(first));
    present(*surface, solidFrame(WHITE, 640, 360));
    QImage image = shot(*surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));

    QWidget second;
    surface->setParent(&second);
    surface->setGeometry(0, 0, 320, 180);
    surface->show();
    QVERIFY(shown(second));
    image = shot(*surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));

    present(*surface, solidFrame(RED_BT709, 1280, 720));
    image = shot(*surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));

#ifdef CARD_FORMAT
    if (surface->sink()->acceptsHardwareFrames(CARD_FORMAT)) {
        if (FramePointer frame = solidFrameOnCard(WHITE, 640, 360)) {
            present(*surface, std::move(frame));
            QTRY_VERIFY(surface->drawsFromCard());

            surface->setParent(&first);
            surface->setGeometry(0, 0, 320, 180);
            surface->show();
            QTRY_VERIFY(isNear(shot(*surface).pixel(160, 90), 255, 255, 255));
        }
    }
#endif
}

void TestVideoSurface::clickAndDoubleClickAreReported()
{
    VideoSurface surface;
    QVERIFY(shown(surface));

    QSignalSpy clicks(&surface, &VideoSurface::clicked);
    QSignalSpy doubleClicks(&surface, &VideoSurface::doubleClicked);
    QTest::mouseClick(&surface, Qt::LeftButton, {}, QPoint(100, 60));
    QCOMPARE(clicks.count(), 1);
    QTest::mouseClick(&surface, Qt::RightButton, {}, QPoint(100, 60));
    QCOMPARE(clicks.count(), 1);
    QTest::mouseDClick(&surface, Qt::LeftButton, {}, QPoint(100, 60));
    QCOMPARE(doubleClicks.count(), 1);

    if (!surface.drawsWithGpu())
        return;

    drawOnce(surface);
    const QList<QWindow *> layers = layersOf(surface);
    QCOMPARE(layers.size(), 1);
    QWindow *layer = layers.first();

    surface.setContextMenuPolicy(Qt::CustomContextMenu);
    QSignalSpy menus(&surface, &QWidget::customContextMenuRequested);
    const int clicksBefore = clicks.count();
    QTest::mouseClick(layer, Qt::LeftButton, {}, QPoint(100, 60));
    QCOMPARE(clicks.count(), clicksBefore + 1);
    QCOMPARE(menus.count(), 0);
    QTest::mouseClick(layer, Qt::RightButton, {}, QPoint(100, 60));
    QCOMPARE(clicks.count(), clicksBefore + 1);
    QCOMPARE(menus.count(), 1);
    QCOMPARE(menus.first().first().toPoint(), QPoint(100, 60));
    QTest::mouseDClick(layer, Qt::LeftButton, {}, QPoint(100, 60));
    QCOMPARE(doubleClicks.count(), 2);
}

void TestVideoSurface::geometryIsReported()
{
    VideoSurface surface;
    QSignalSpy geometry(&surface, &VideoSurface::geometryChanged);
    QVERIFY(shown(surface));

    const qreal ratio = surface.devicePixelRatioF();
    const int fullArea = qRound(320 * ratio) * qRound(180 * ratio);

    QVERIFY(!geometry.isEmpty());
    QCOMPARE(geometry.last().at(0).toInt(), fullArea);
    QCOMPARE(geometry.last().at(1).toBool(), true);

    present(surface, solidFrame(WHITE, 640, 480));
    QCOMPARE(geometry.last().at(0).toInt(), qRound(240 * ratio) * qRound(180 * ratio));

    surface.hide();
    QCOMPARE(geometry.last().at(0).toInt(), 0);
    QCOMPARE(geometry.last().at(1).toBool(), false);
}

void TestVideoSurface::resizeRedrawsTheLastFrame()
{
    // inside a window: on Wayland the compositor sets a top-level window's size and sets it back
    QWidget window;
    VideoSurface surface(&window);
    surface.setGeometry(0, 0, 320, 180);
    QVERIFY(shown(window, QSize(480, 270)));
    present(surface, solidFrame(WHITE, 640, 360));

    surface.resize(480, 270);
    QTest::qWait(300);

    const QImage image = shot(surface);
    QCOMPARE(image.size(), QSize(480, 270) * image.devicePixelRatio());
    QVERIFY(isNear(image.pixel(1, 1), 255, 255, 255));
    QVERIFY(isNear(image.pixel(image.width() - 2, image.height() - 2), 255, 255, 255));
}

void TestVideoSurface::messagesDoNotDisturbThePicture()
{
    VideoSurface surface;
    QVERIFY(shown(surface));
    present(surface, solidFrame(WHITE, 640, 360));

    surface.setStatsText(QStringLiteral("Codec: H264"));
    QImage image = shot(surface);
    QVERIFY(!isNear(image.pixel(qRound(10 * image.devicePixelRatio()), qRound(10 * image.devicePixelRatio())), 255, 255, 255));
    QVERIFY(isNear(image.pixel(image.width() - 2, image.height() - 2), 255, 255, 255));

    surface.setStatsText({});
    surface.setMessage(QStringLiteral("Stream Paused"), QStringLiteral("sit tight"));
    image = shot(surface);
    QVERIFY(!isNear(image.pixel(image.width() - 2, image.height() - 2), 255, 255, 255));

    surface.setMessage({});
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() - 2, image.height() - 2), 255, 255, 255));
}

void TestVideoSurface::avatarStandsInUntilThereIsAPicture()
{
    VideoSurface surface;
    QVERIFY(shown(surface));

    QPixmap avatar(64, 64);
    avatar.fill(QColor(0, 200, 0));
    surface.setPlaceholder(avatar);

    QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 0, 200, 0));
    QVERIFY(isNear(image.pixel(4, 4), 0, 0, 0));

    present(surface, solidFrame(WHITE, 640, 360));
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));

    surface.clear();
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 0, 200, 0));
}

void TestVideoSurface::speakingRingIsDrawn()
{
    VideoSurface surface;
    QVERIFY(shown(surface));

    QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(1, image.height() / 2), 0, 0, 0));

    surface.setSpeaking(true);
    image = shot(surface);
    QVERIFY(!isNear(image.pixel(1, image.height() / 2), 0, 0, 0));
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 0, 0, 0));

    surface.setSpeaking(false);
    image = shot(surface);
    QVERIFY(isNear(image.pixel(1, image.height() / 2), 0, 0, 0));

    // the corners are left out: the painter cuts them off and the graphics card doesnt
    present(surface, solidFrame(WHITE, 640, 360));
    surface.setSpeaking(true);
    image = shot(surface);
    const int right = image.width() - 1;
    const int bottom = image.height() - 1;
    for (const QPoint &onTheRing : { QPoint(0, bottom / 2), QPoint(right, bottom / 2), QPoint(right / 2, 0), QPoint(right / 2, bottom) })
        QVERIFY2(isNear(image.pixel(onTheRing), surface.speakingRingColor()), qPrintable(QStringLiteral("%1,%2").arg(onTheRing.x()).arg(onTheRing.y())));
    const int inside = qRound(4 * image.devicePixelRatio());
    for (const QPoint &offIt : { QPoint(inside, bottom / 2), QPoint(right - inside, bottom / 2), QPoint(right / 2, inside), QPoint(right / 2, bottom - inside), QPoint(right / 2, bottom / 2) })
        QVERIFY2(isNear(image.pixel(offIt), 255, 255, 255), qPrintable(QStringLiteral("%1,%2").arg(offIt.x()).arg(offIt.y())));

    surface.setSpeaking(false);
    image = shot(surface);
    QVERIFY(isNear(image.pixel(0, bottom / 2), 255, 255, 255));
}

void TestVideoSurface::aSurfaceHasItsLayerOnlyWhileItHasAPicture()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    VideoSurface surface;
    QVERIFY(shown(surface));
    if (!surface.drawsWithGpu())
        QSKIP("This surface draws without the graphics card");

    QPixmap avatar(64, 64);
    avatar.fill(QColor(0, 200, 0));
    surface.setPlaceholder(avatar);
    surface.setCaption(QStringLiteral("Somebody"));
    surface.setSpeaking(true);
    QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 0, 200, 0));
    QVERIFY(isNear(image.pixel(1, image.height() / 2), surface.speakingRingColor()));
    QCOMPARE(layersOf(surface).size(), 0);

    present(surface, solidFrame(WHITE, 640, 360));
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));
    QVERIFY(isNear(image.pixel(1, image.height() / 2), surface.speakingRingColor()));
    QCOMPARE(layersOf(surface).size(), 1);

    surface.clear();
    QCOMPARE(layersOf(surface).size(), 0);
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 0, 200, 0));

    present(surface, solidFrame(RED_BT709, 1280, 720));
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));
    QCOMPARE(layersOf(surface).size(), 1);
}

void TestVideoSurface::aSurfaceLongOffTheScreenGivesUpItsLayerAndKeepsItsPicture()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    VideoSurface surface;
    QVERIFY(shown(surface));
    if (!surface.drawsWithGpu())
        QSKIP("This surface draws without the graphics card");

    FramePointer picture;
#ifdef CARD_FORMAT
    if (takesCardPictures(surface))
        picture = solidFrameOnCard(RED_BT709, 1280, 720);
#endif
    const bool fromCard = picture != nullptr;
    present(surface, fromCard ? std::move(picture) : solidFrame(RED_BT709, 1280, 720));
    if (fromCard)
        QTRY_VERIFY(surface.drawsFromCard());
    QImage image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));
    QCOMPARE(layersOf(surface).size(), 1);

    surface.hide();
    QTest::qWait(500);
    QCOMPARE(layersOf(surface).size(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(layersOf(surface).size(), 0, VideoSurface::NOT_SHOWN_BEFORE_RELEASED_MS);

    surface.show();
    QVERIFY(QTest::qWaitForWindowExposed(&surface));
    image = shot(surface);
    QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0));
    QCOMPARE(layersOf(surface).size(), 1);
    QVERIFY(!surface.drawsFromCard());
}

void TestVideoSurface::statisticsAreLeftOffASmallTile()
{
    const QString stats = QStringLiteral("Codec: H264\nResolution: 1280x720\nFrame rate: 30.0 fps\nBitrate: 900 kbps");

    VideoSurface large;
    QVERIFY(shown(large, QSize(480, 270)));
    present(large, solidFrame(WHITE, 640, 360));
    large.setStatsText(stats);
    large.setCaption(QStringLiteral("caption text"));
    QImage image = shot(large);
    QVERIFY(!isNear(image.pixel(5, 5), 255, 255, 255));

    VideoSurface small;
    QVERIFY(shown(small, QSize(160, 90)));
    present(small, solidFrame(WHITE, 640, 360));
    small.setStatsText(stats);
    image = shot(small);
    QVERIFY(isNear(image.pixel(5, 5), 255, 255, 255));
}

void TestVideoSurface::pictureFromAnotherCardIsReadBack()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#ifdef HAVE_VAAPI
    const QList<QByteArray> nodes = renderNodes();
    for (const QByteArray &node : nodes) {
        if (node == eglHere().renderNode)
            continue;
        VideoSurface surface;
        QVERIFY(shown(surface));
        if (!takesCardPictures(surface))
            QSKIP(CANNOT_DRAW_FROM_CARD);
        FramePointer frame = solidFrameOnNode(node, RED_BT709, 1280, 720);
        if (!frame)
            continue;

        present(surface, std::move(frame));
        QImage image = shot(surface);
        QVERIFY2(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 0, 0), node.constData());
        if (surface.drawsFromCard())
            continue;

        QVERIFY(!surface.sink()->acceptsHardwareFrames(CARD_FORMAT));
        present(surface, solidFrame(WHITE, 640, 360));
        image = shot(surface);
        QVERIFY(isNear(image.pixel(image.width() / 2, image.height() / 2), 255, 255, 255));

        VideoSurface later;
        QVERIFY(shown(later));
        QVERIFY(!takesCardPictures(later));
        return;
    }
    QSKIP("No second graphics card whose pictures the first cannot draw where they are");
#else
    QSKIP("VAAPI only");
#endif
}

class OutOfTheWay : public QObject
{
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *window = qobject_cast<QWidget *>(watched);
        if (event->type() != QEvent::Polish || !window || !window->isWindow())
            return false;

        window->setAttribute(Qt::WA_ShowWithoutActivating);
        const QList<QScreen *> screens = QGuiApplication::screens();
        const auto another = std::find_if(screens.begin(), screens.end(), [](const QScreen *screen) { return screen != QGuiApplication::primaryScreen(); });
        if (another != screens.end())
            window->move((*another)->availableGeometry().topLeft() + QPoint(48, 48));
        return false;
    }
};

int main(int argc, char **argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    OutOfTheWay outOfTheWay;
    app.installEventFilter(&outOfTheWay);
    TestVideoSurface test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_VideoSurface.moc"
