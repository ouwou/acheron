#include "Core/Media/InputConfinement.hpp"
#include "Core/Media/RgbScaler.hpp"

#include <QColor>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <cstring>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

using namespace Acheron::Core::Media;

namespace {

constexpr int CHANNEL_TOLERANCE = 6;

struct Yuv
{
    uint8_t y;
    uint8_t u;
    uint8_t v;
};

constexpr Yuv RED_BT709{ 63, 102, 240 };
constexpr Yuv GREEN_BT709{ 173, 42, 26 };
constexpr Yuv RED_BT601{ 81, 90, 240 };

AVFrame *solidFrame(Yuv colour, int width, int height)
{
    AVFrame *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 0);
    for (int row = 0; row < height; row++)
        memset(frame->data[0] + row * frame->linesize[0], colour.y, static_cast<size_t>(width));
    for (int row = 0; row < (height + 1) / 2; row++) {
        memset(frame->data[1] + row * frame->linesize[1], colour.u, static_cast<size_t>((width + 1) / 2));
        memset(frame->data[2] + row * frame->linesize[2], colour.v, static_cast<size_t>((width + 1) / 2));
    }
    return frame;
}

QColor shownColour(RgbScaler &scaler, const AVFrame *frame, QSize size = QSize(16, 16))
{
    QImage image = RgbScaler::allocate(size, QImage::Format_RGB32);
    if (!scaler.scale(frame, image))
        return {};
    return image.pixelColor(size.width() / 2, size.height() / 2);
}

bool closeTo(const QColor &shown, const QColor &wanted)
{
    return shown.isValid() && qAbs(shown.red() - wanted.red()) <= CHANNEL_TOLERANCE && qAbs(shown.green() - wanted.green()) <= CHANNEL_TOLERANCE && qAbs(shown.blue() - wanted.blue()) <= CHANNEL_TOLERANCE;
}

class PlaylistServer : public QObject
{
public:
    PlaylistServer()
    {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = server.nextPendingConnection())
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { answer(socket); });
        });
        server.listen(QHostAddress::LocalHost);
    }

    QString playlistUrl() const { return QStringLiteral("http://127.0.0.1:%1/attachments/clip.m3u8").arg(server.serverPort()); }

    int playlistRequests = 0;
    int segmentRequests = 0;

private:
    void answer(QTcpSocket *socket)
    {
        const QByteArray request = socket->readAll();
        if (request.startsWith("GET /elsewhere/")) {
            segmentRequests++;
            socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        } else {
            playlistRequests++;
            const QByteArray playlist = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n#EXTINF:1.0,\nhttp://127.0.0.1:" + QByteArray::number(server.serverPort()) + "/elsewhere/segment.ts\n#EXT-X-ENDLIST\n";
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nConnection: close\r\nContent-Length: " + QByteArray::number(playlist.size()) + "\r\n\r\n" + playlist);
        }
        socket->disconnectFromHost();
    }

    QTcpServer server;
};

bool openAndClose(const QString &url, bool confined)
{
    std::atomic<bool> done = false;
    std::thread opener([&] {
        InputConfinement confinement;
        AVFormatContext *format = avformat_alloc_context();
        AVDictionary *options = nullptr;
        if (confined)
            confinement.apply(format, &options, url);
        av_dict_set(&options, "rw_timeout", "3000000", 0);
        if (avformat_open_input(&format, url.toUtf8().constData(), nullptr, &options) >= 0)
            avformat_close_input(&format);
        av_dict_free(&options);
        done = true;
    });
    const bool finished = QTest::qWaitFor([&] { return done.load(); }, 20000);
    opener.join();
    return finished;
}

} // namespace

class TestMediaInput : public QObject
{
    Q_OBJECT
private slots:
    void bt709IsShownInItsOwnColours();
    void bt601IsShownInItsOwnColours();
    void untaggedVideoGoesBySize();
    void fullRangeIsNotStretched();
    void aChangeOfColoursPartWayIsFollowed();
    void anOddSizedPictureFitsItsImage();

    void aPlaylistLeftAloneFetchesWhatItNames();
    void aConfinedPlaylistFetchesNothingElse();
    void confinementLeavesAnOrdinaryFilePlayable();
};

void TestMediaInput::bt709IsShownInItsOwnColours()
{
    RgbScaler scaler;
    AVFrame *red = solidFrame(RED_BT709, 64, 64);
    red->colorspace = AVCOL_SPC_BT709;
    AVFrame *green = solidFrame(GREEN_BT709, 64, 64);
    green->colorspace = AVCOL_SPC_BT709;

    const QColor shownRed = shownColour(scaler, red);
    const QColor shownGreen = shownColour(scaler, green);
    QVERIFY2(closeTo(shownRed, QColor(255, 0, 0)), qPrintable(shownRed.name()));
    QVERIFY2(closeTo(shownGreen, QColor(0, 255, 0)), qPrintable(shownGreen.name()));

    av_frame_free(&red);
    av_frame_free(&green);
}

void TestMediaInput::bt601IsShownInItsOwnColours()
{
    RgbScaler scaler;
    AVFrame *red = solidFrame(RED_BT601, 64, 64);
    red->colorspace = AVCOL_SPC_SMPTE170M;

    const QColor shown = shownColour(scaler, red);
    QVERIFY2(closeTo(shown, QColor(255, 0, 0)), qPrintable(shown.name()));
    av_frame_free(&red);
}

void TestMediaInput::untaggedVideoGoesBySize()
{
    RgbScaler scaler;
    AVFrame *small = solidFrame(RED_BT601, 64, 64);
    AVFrame *large = solidFrame(RED_BT709, 1280, 720);

    const QColor shownSmall = shownColour(scaler, small);
    // swscale's bilinear filter loses a little brightness once a picture is reduced some twentyfold
    const QColor shownLarge = shownColour(scaler, large, QSize(320, 180));
    QVERIFY2(closeTo(shownSmall, QColor(255, 0, 0)), qPrintable(shownSmall.name()));
    QVERIFY2(closeTo(shownLarge, QColor(255, 0, 0)), qPrintable(shownLarge.name()));

    av_frame_free(&small);
    av_frame_free(&large);
}

void TestMediaInput::fullRangeIsNotStretched()
{
    RgbScaler scaler;
    constexpr Yuv LIGHT_GREY{ 235, 128, 128 };
    AVFrame *full = solidFrame(LIGHT_GREY, 64, 64);
    full->color_range = AVCOL_RANGE_JPEG;
    AVFrame *video = solidFrame(LIGHT_GREY, 64, 64);
    video->color_range = AVCOL_RANGE_MPEG;

    const QColor shownFull = shownColour(scaler, full);
    const QColor shownVideo = shownColour(scaler, video);
    QVERIFY2(closeTo(shownFull, QColor(235, 235, 235)), qPrintable(shownFull.name()));
    QVERIFY2(closeTo(shownVideo, QColor(255, 255, 255)), qPrintable(shownVideo.name()));

    av_frame_free(&full);
    av_frame_free(&video);
}

void TestMediaInput::aChangeOfColoursPartWayIsFollowed()
{
    RgbScaler scaler;
    AVFrame *frame = solidFrame(RED_BT709, 64, 64);
    frame->colorspace = AVCOL_SPC_BT709;
    QVERIFY(closeTo(shownColour(scaler, frame), QColor(255, 0, 0)));

    AVFrame *next = solidFrame(RED_BT601, 64, 64);
    next->colorspace = AVCOL_SPC_SMPTE170M;
    const QColor shown = shownColour(scaler, next);
    QVERIFY2(closeTo(shown, QColor(255, 0, 0)), qPrintable(shown.name()));

    av_frame_free(&frame);
    av_frame_free(&next);
}

void TestMediaInput::anOddSizedPictureFitsItsImage()
{
    RgbScaler scaler;
    AVFrame *frame = solidFrame(RED_BT601, 70, 38);
    for (const QSize size : { QSize(1, 1), QSize(3, 5), QSize(17, 9), QSize(123, 67), QSize(641, 359) }) {
        const QColor shown = shownColour(scaler, frame, size);
        QVERIFY2(closeTo(shown, QColor(255, 0, 0)), qPrintable(QStringLiteral("%1x%2 %3").arg(size.width()).arg(size.height()).arg(shown.name())));
    }
    av_frame_free(&frame);
}

void TestMediaInput::aPlaylistLeftAloneFetchesWhatItNames()
{
    PlaylistServer server;
    QVERIFY(openAndClose(server.playlistUrl(), false));
    QCOMPARE(server.playlistRequests, 1);
    QVERIFY(server.segmentRequests >= 1);
}

void TestMediaInput::aConfinedPlaylistFetchesNothingElse()
{
    PlaylistServer server;
    QVERIFY(openAndClose(server.playlistUrl(), true));
    QCOMPARE(server.playlistRequests, 1);
    QCOMPARE(server.segmentRequests, 0);
}

void TestMediaInput::confinementLeavesAnOrdinaryFilePlayable()
{
    // a WAV header and a second of 8-bit silence at 8 kHz
    QByteArray wav = QByteArray::fromHex("52494646641f000057415645666d7420100000000100010040"
                                         "1f0000401f00000100080064617461401f0000");
    wav.append(QByteArray(8000, char(0x80)));

    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("silence.wav"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(wav);
    file.close();

    InputConfinement confinement;
    AVFormatContext *format = avformat_alloc_context();
    AVDictionary *options = nullptr;
    confinement.apply(format, &options, path);
    const int opened = avformat_open_input(&format, path.toUtf8().constData(), nullptr, &options);
    av_dict_free(&options);
    QVERIFY(opened >= 0);
    QCOMPARE(QByteArray(format->iformat->name), QByteArray("wav"));
    avformat_close_input(&format);
}

QTEST_GUILESS_MAIN(TestMediaInput)
#include "tst_MediaInput.moc"
