// what the real camera sees is only counted and measured here: never printed, written or shown

#include "Core/Media/RgbScaler.hpp"
#include "Core/Media/VideoDecoder.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/CameraCapture.hpp"
#include "Core/Video/CameraPipeline.hpp"
#include "ColourChecks.hpp"
#include "EncodedFrameChecks.hpp"
#include "ProcessUsage.hpp"
#include "HardwareTests.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#ifdef Q_OS_WIN
#include <windows.h>
#endif
#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using namespace Acheron;
using namespace Acheron::Core::Video;
using Discord::Voice::EncodedVideoFrame;
using Discord::Voice::VideoCodec;
using Discord::Voice::videoCodecName;
using namespace EncodedFrameChecks;
using ProcessUsage::processCpuMs;

namespace {

using ColourChecks::distance;
using ColourChecks::inYuv;
using ColourChecks::Yuv;
using Clock = std::chrono::steady_clock;

constexpr int BITRATE = 1'500'000;
constexpr int TICKS_PER_SECOND = 90000;
constexpr int REAL_CAMERA_MS = 1900;

constexpr int COLOUR_RED = 40;
constexpr int COLOUR_GREEN = 200;
constexpr int COLOUR_BLUE = 60;
constexpr int DARK_GREY = 60;
constexpr int LIGHT_GREY = 200;
constexpr int SQUARE_LUMA = 220;
constexpr int AROUND_SQUARE_LUMA = 40;

using FramePointer = std::shared_ptr<AVFrame>;

FramePointer makeFrame()
{
    return FramePointer(av_frame_alloc(), [](AVFrame *frame) { av_frame_free(&frame); });
}

struct SyntheticCamera
{
    enum class Shows {
        Movement,
        Square,
        TwoGreys,
        OneColour,
    };

    QSize size = QSize(640, 360);
    AVPixelFormat format = AV_PIX_FMT_YUV420P;
    bool fullRange = false;
    AVColorSpace colorspace = AVCOL_SPC_SMPTE170M;
    int framerate = 30;
    Shows shows = Shows::Movement;
    bool opens = true;
    int opensAfterMs = 0;
    std::optional<int> lostAfter;

    std::atomic<int> timesOpened{ 0 };
    std::atomic<int> openNow{ 0 };
    std::atomic<int> picturesGiven{ 0 };
    std::mutex mutex;
    QString openedWithId;

    void atComingPictures(std::initializer_list<std::function<void()>> each)
    {
        std::lock_guard lock(mutex);
        dueAtComingPictures.insert(dueAtComingPictures.end(), each);
    }

    void doWhatIsDueAtThisPicture()
    {
        std::function<void()> due;
        {
            std::lock_guard lock(mutex);
            if (dueAtComingPictures.empty())
                return;
            due = std::move(dueAtComingPictures.front());
            dueAtComingPictures.pop_front();
        }
        due();
    }

private:
    std::deque<std::function<void()>> dueAtComingPictures;
};

void paint(AVFrame *picture, const SyntheticCamera &camera, int index)
{
    const AVPixFmtDescriptor *layout = av_pix_fmt_desc_get(camera.format);
    const int width = picture->width;
    const int height = picture->height;
    const int chromaWidth = AV_CEIL_RSHIFT(width, layout->log2_chroma_w);
    const int chromaHeight = AV_CEIL_RSHIFT(height, layout->log2_chroma_h);
    const Yuv colour = inYuv(COLOUR_RED, COLOUR_GREEN, COLOUR_BLUE, camera.colorspace == AVCOL_SPC_BT709, camera.fullRange);
    const int squareSide = height / 3;

    uint32_t noise = 12345u + static_cast<uint32_t>(index) * 2654435761u;
    for (int y = 0; y < height; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        for (int x = 0; x < width; x++) {
            uint8_t value = 0;
            switch (camera.shows) {
            case SyntheticCamera::Shows::Movement:
                value = static_cast<uint8_t>((x + y) / 4 + index * 3);
                if (x >= width / 4 && x < width * 3 / 4 && y >= height / 3 && y < height * 2 / 3)
                    value = static_cast<uint8_t>(((x + 3 * index) * 7) ^ ((y + 2 * index) * 5));
                if (x < width / 4 && y < height / 6) {
                    noise = noise * 1664525u + 1013904223u;
                    value = static_cast<uint8_t>(96 + (noise >> 26));
                }
                break;
            case SyntheticCamera::Shows::Square:
                value = std::abs(2 * x - width) < squareSide && std::abs(2 * y - height) < squareSide ? SQUARE_LUMA : AROUND_SQUARE_LUMA;
                break;
            case SyntheticCamera::Shows::TwoGreys:
                value = x < width / 2 ? DARK_GREY : LIGHT_GREY;
                break;
            case SyntheticCamera::Shows::OneColour:
                value = static_cast<uint8_t>(std::lround(colour.y));
                break;
            }
            row[x] = value;
        }
    }

    for (int y = 0; y < chromaHeight; y++) {
        for (int x = 0; x < chromaWidth; x++) {
            uint8_t u = 128;
            uint8_t v = 128;
            if (camera.shows == SyntheticCamera::Shows::Movement) {
                u = static_cast<uint8_t>(128 + y / 4 + index * 2);
                v = static_cast<uint8_t>(64 + x / 4 + index * 5);
            } else if (camera.shows == SyntheticCamera::Shows::OneColour) {
                u = static_cast<uint8_t>(std::lround(colour.u));
                v = static_cast<uint8_t>(std::lround(colour.v));
            }
            picture->data[1][y * picture->linesize[1] + x] = u;
            picture->data[2][y * picture->linesize[2] + x] = v;
        }
    }
}

class SyntheticSource : public CameraFrameSource
{
public:
    explicit SyntheticSource(std::shared_ptr<SyntheticCamera> camera) : camera(std::move(camera)) {}

    ~SyntheticSource() override
    {
        if (isOpen)
            camera->openNow--;
    }

    bool open(const QString &deviceId) override
    {
        {
            std::lock_guard lock(camera->mutex);
            camera->openedWithId = deviceId;
        }
        camera->timesOpened++;
        std::this_thread::sleep_for(std::chrono::milliseconds(camera->opensAfterMs));
        if (!camera->opens)
            return false;

        const int different = camera->shows == SyntheticCamera::Shows::Movement ? 8 : 1;
        for (int index = 0; index < different; index++) {
            FramePointer picture = makeFrame();
            picture->format = camera->format;
            picture->width = camera->size.width();
            picture->height = camera->size.height();
            picture->colorspace = camera->colorspace;
            picture->color_range = camera->fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
            if (av_frame_get_buffer(picture.get(), 0) < 0)
                return false;
            paint(picture.get(), *camera, index);
            pictures.push_back(std::move(picture));
        }

        isOpen = true;
        camera->openNow++;
        openedAt = Clock::now();
        return true;
    }

    Status read(AVFrame *into, Clock::time_point &capturedAt) override
    {
        if (camera->lostAfter && given >= *camera->lostAfter)
            return Status::Ended;
        camera->doWhatIsDueAtThisPicture();

        const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::microseconds(1'000'000 / camera->framerate));
        const qint64 newestTaken = (Clock::now() - openedAt) / interval;
        const qint64 index = std::max(lastGiven + 1, newestTaken);
        const Clock::time_point takenAt = openedAt + index * interval;
        std::this_thread::sleep_until(takenAt);

        av_frame_ref(into, pictures[static_cast<size_t>(index) % pictures.size()].get());
        capturedAt = takenAt;
        lastGiven = index;
        given++;
        camera->picturesGiven++;
        return Status::Frame;
    }

    QString failure() const override { return isOpen ? QStringLiteral("the synthetic camera was lost") : QStringLiteral("the synthetic camera does not open"); }
    QString description() const override { return QStringLiteral("a synthetic camera"); }

private:
    std::shared_ptr<SyntheticCamera> camera;
    std::vector<FramePointer> pictures;
    bool isOpen = false;
    Clock::time_point openedAt;
    qint64 lastGiven = -1;
    int given = 0;
};

class KeepingSink : public Core::Media::VideoFrameSink
{
public:
    void present(const AVFrame *frame) override
    {
        std::lock_guard lock(mutex);
        const QSize size(frame->width, frame->height);
        if (sizes.isEmpty() || sizes.last() != size)
            sizes.append(size);
        av_frame_unref(last.get());
        av_frame_ref(last.get(), frame);
        frames++;
    }

    std::atomic<int> frames{ 0 };
    std::mutex mutex;
    QList<QSize> sizes;
    FramePointer last = makeFrame();
};

struct Decoded
{
    int pictures = 0;
    quint64 errors = 0;
    quint64 dropped = 0;
    QList<QSize> sizes;
    FramePointer last;
};

Decoded decodeAll(VideoCodec codec, const std::vector<EncodedFramePointer> &frames, size_t from = 0, size_t to = SIZE_MAX)
{
    const auto sink = std::make_shared<KeepingSink>();
    Decoded decoded;
    int pushed = 0;
    {
        Core::Media::VideoDecoder decoder(false);
        decoder.setSink(sink);

        QElapsedTimer timer;
        timer.start();
        for (size_t i = from; i < std::min(to, frames.size()); i++) {
            decoder.push(codec, EncodedVideoFrame(*frames[i]));
            pushed++;
            while (pushed - sink->frames > 20 && timer.elapsed() < 10000)
                QThread::msleep(1);
        }
        while (sink->frames < pushed && timer.elapsed() < 10000)
            QThread::msleep(2);
        QThread::msleep(50);
        decoded.errors = decoder.stats().errors;
        decoded.dropped = decoder.stats().dropped;
    }
    decoded.pictures = sink->frames;
    decoded.sizes = sink->sizes;
    decoded.last = sink->last;
    return decoded;
}

bool isPlanar420(const AVFrame *frame)
{
    return frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P;
}

Yuv middleOf(const AVFrame *frame)
{
    const int x = frame->width / 2;
    const int y = frame->height / 2;
    return { static_cast<double>(frame->data[0][y * frame->linesize[0] + x]), static_cast<double>(frame->data[1][(y / 2) * frame->linesize[1] + x / 2]), static_cast<double>(frame->data[2][(y / 2) * frame->linesize[2] + x / 2]) };
}

QSize brightExtent(const AVFrame *frame)
{
    constexpr int BRIGHT = (SQUARE_LUMA + AROUND_SQUARE_LUMA) / 2;
    int across = 0;
    int down = 0;
    for (int x = 0; x < frame->width; x++)
        across += frame->data[0][(frame->height / 2) * frame->linesize[0] + x] > BRIGHT ? 1 : 0;
    for (int y = 0; y < frame->height; y++)
        down += frame->data[0][y * frame->linesize[0] + frame->width / 2] > BRIGHT ? 1 : 0;
    return QSize(across, down);
}

QImage inRgb(const AVFrame *frame)
{
    QImage image = Core::Media::RgbScaler::allocate(QSize(frame->width, frame->height), QImage::Format_RGB32);
    Core::Media::RgbScaler scaler;
    return scaler.scale(frame, image) ? image : QImage();
}

bool isGrey(QRgb pixel, int grey, int within)
{
    return std::abs(qRed(pixel) - grey) <= within && std::abs(qGreen(pixel) - grey) <= within && std::abs(qBlue(pixel) - grey) <= within;
}

QString describe(QRgb pixel)
{
    return QStringLiteral("(%1, %2, %3)").arg(qRed(pixel)).arg(qGreen(pixel)).arg(qBlue(pixel));
}

bool arePicturesOfTheSizeSent(VideoCodec codec, const QList<QSize> &decoded, const QSize &sent)
{
    if (decoded.size() != 1)
        return false;
    const QSize beyond = decoded.first() - sent;
    return beyond == QSize(0, 0) || (codec == VideoCodec::AV1 && beyond.width() >= 0 && beyond.width() < 64 && beyond.height() >= 0 && beyond.height() < 16);
}

struct PipelineRecord
{
    struct Start
    {
        QSize size;
        int framerate = 0;

        bool operator==(const Start &) const = default;
    };

    explicit PipelineRecord(CameraPipeline &pipeline)
    {
        clock.start();
        QObject::connect(&pipeline, &CameraPipeline::started, &here, [this](const QSize &size, int framerate, int layer) { (layer == 0 ? starts : secondStarts).append({ size, framerate }); });
        QObject::connect(&pipeline, &CameraPipeline::frameEncoded, &here, [this](const EncodedFramePointer &frame, int layer) {
            if (layer != 0) {
                secondFrames.push_back(frame);
                return;
            }
            frames.push_back(frame);
            if (firstFrameAt < 0)
                firstFrameAt = clock.elapsed();
        });
        QObject::connect(&pipeline, &CameraPipeline::previewReady, &here, [this](const QImage &preview) { previews.append(preview); });
        QObject::connect(&pipeline, &CameraPipeline::statsUpdated, &here, [this](const PipelineStats &update, int layer) {
            if (layer != 0)
                return;
            stats.append(update);
            statsAt.append(clock.elapsed());
        });
        QObject::connect(&pipeline, &CameraPipeline::failed, &here, [this](const QString &reason) { failures.append(reason); });
    }

    [[nodiscard]] size_t keyFrameFrom(size_t from) const
    {
        size_t index = from;
        while (index < frames.size() && !frames[index]->keyframe)
            index++;
        return index;
    }

    [[nodiscard]] int keyFramesFrom(size_t from) const
    {
        return static_cast<int>(std::count_if(frames.begin() + static_cast<std::ptrdiff_t>(from), frames.end(), [](const EncodedFramePointer &frame) { return frame->keyframe; }));
    }

    [[nodiscard]] std::vector<int> ticksBetween(size_t from = 0) const
    {
        std::vector<int> ticks;
        for (size_t i = from + 1; i < frames.size(); i++)
            ticks.push_back(static_cast<int32_t>(frames[i]->rtpTimestamp - frames[i - 1]->rtpTimestamp));
        return ticks;
    }

    QObject here;
    QElapsedTimer clock;
    qint64 firstFrameAt = -1;
    std::vector<EncodedFramePointer> frames;
    std::vector<EncodedFramePointer> secondFrames;
    QList<Start> secondStarts;
    QList<QImage> previews;
    QList<Start> starts;
    QList<PipelineStats> stats;
    QList<qint64> statsAt;
    QStringList failures;
};

double framesPerSecond(const std::vector<int> &ticksBetween)
{
    qint64 total = 0;
    for (int ticks : ticksBetween)
        total += ticks;
    return total > 0 ? static_cast<double>(ticksBetween.size()) * TICKS_PER_SECOND / static_cast<double>(total) : 0;
}

CameraSettings settingsFor(VideoCodec codec, const QSize &encodeSize, int framerate)
{
    CameraSettings settings;
    settings.deviceId = QStringLiteral("the camera of the test's own");
    settings.codec = codec;
    settings.encodeSize = encodeSize;
    settings.framerate = framerate;
    settings.bitrateBps = BITRATE;
    settings.maxBitrateBps = 4'000'000;
    return settings;
}

} // namespace

Q_DECLARE_METATYPE(VideoCodec)
Q_DECLARE_METATYPE(AVColorSpace)

#define SKIP_WITHOUT_AN_ENCODER(codec)                                       \
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs(); \
    if (encodable.isEmpty())                                                 \
        QSKIP("this machine has no encoder");                                \
    const VideoCodec codec = encodable.contains(VideoCodec::H264) ? VideoCodec::H264 : encodable.first()

#define SKIP_A_CODEC_THAT_IS_NOT_ENCODED(codec) \
    if ((codec) == VideoCodec::Unknown)         \
    QSKIP("this machine has no encoder")

class TestCameraCapture : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void cleanup();

    void camerasAreListedTheSameEveryTime();
    void aCamerasOwnClockSaysWhenAPictureWasTaken();
    void aCameraClockThatRunsSlowIsFollowed();
    void aCameraClockThatStartsOverIsTakenUpAgain();

    void picturesAreEncodedAtTheSizeAskedFor_data() { everyCodec(); }
    void picturesAreEncodedAtTheSizeAskedFor();
    void aCameraOfAnotherShapeKeepsItsShape_data();
    void aCameraOfAnotherShapeKeepsItsShape();
    void picturesAreThinnedToTheFrameRate_data();
    void picturesAreThinnedToTheFrameRate();
    void aNewSizeAndRateStartWithAKeyFrame();
    void aNewBitrateAndSizeTogetherCostOneKeyFrame();
    void whatIsAskedForBeforeTheFirstFrameShapesIt_data();
    void whatIsAskedForBeforeTheFirstFrameShapesIt();
    void aKeyFrameComesWhenAskedFor();
    void nothingIsEncodedWhileSuspended();
    void aCameraOpenedWhileSuspendedIsAnnounced();
    void aSecondLayerIsEncodedBesideTheFirstOnceItIsAskedFor();
    void aFullRangeCameraKeepsItsBrightness_data() { everyCodec(); }
    void aFullRangeCameraKeepsItsBrightness();
    void aCamerasColoursAreWhatAViewerTakesThemFor_data();
    void aCamerasColoursAreWhatAViewerTakesThemFor();
    void thePreviewIsSmallAndFrequent_data();
    void thePreviewIsSmallAndFrequent();
    void theBitrateIsFollowed();
    void statisticsComeEverySecond();
    void aCameraThatIsLostFails();
    void aCameraThatDoesNotOpenFails();
    void noCameraIsOpenedWithoutAnEncoder();
    void stoppingClosesTheCameraAndItOpensAgain();

    void theFirstCameraDeliversPictures();
    void aPictureOutlivesItsCamera();

private:
    static void everyCodec();

    std::shared_ptr<SyntheticCamera> camera;
};

void TestCameraCapture::everyCodec()
{
    QTest::addColumn<VideoCodec>("codec");
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    for (VideoCodec codec : encodable)
        QTest::newRow(qPrintable(videoCodecName(codec))) << codec;
    if (encodable.isEmpty())
        QTest::newRow("no encoder") << VideoCodec::Unknown;
}

void TestCameraCapture::initTestCase()
{
    QStringList codecs;
    for (VideoCodec codec : Core::Media::encodableVideoCodecs())
        codecs.append(videoCodecName(codec));
    qInfo() << "encodable codecs:" << codecs;
}

void TestCameraCapture::init()
{
    camera = std::make_shared<SyntheticCamera>();
    setCameraFrameSourceFactory([made = camera] { return std::make_unique<SyntheticSource>(made); });
}

void TestCameraCapture::cleanup()
{
    setCameraFrameSourceFactory({});
}

void TestCameraCapture::camerasAreListedTheSameEveryTime()
{
    QElapsedTimer timer;
    timer.start();
    const QList<CameraDevice> first = cameraDevices();
    const qint64 firstListingMs = timer.elapsed();

    QSet<QString> ids;
    QStringList names;
    for (const CameraDevice &each : first) {
        QVERIFY(!each.id.isEmpty());
        QVERIFY(!each.name.isEmpty());
        ids.insert(each.id);
        names.append(each.name);
    }
    QCOMPARE(ids.size(), first.size());
    if (!cameraCaptureAvailable())
        QVERIFY(first.isEmpty());

#ifdef Q_OS_LINUX
    QSet<QString> capturing;
    const QStringList nodes = QDir(QStringLiteral("/dev")).entryList({ QStringLiteral("video*") }, QDir::System);
    for (const QString &node : nodes) {
        const QString path = QStringLiteral("/dev/") + node;
        v4l2_capability device{};
        const int fd = open(QFile::encodeName(path).constData(), O_RDONLY);
        if (fd >= 0 && ioctl(fd, VIDIOC_QUERYCAP, &device) == 0 && (device.device_caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) && (device.device_caps & V4L2_CAP_STREAMING))
            capturing.insert(path);
        if (fd >= 0)
            close(fd);
    }
    QSet<QString> listedNodes;
    for (const CameraDevice &each : first)
        listedNodes.insert(QFileInfo(each.id).canonicalFilePath());
    if (cameraCaptureAvailable())
        QCOMPARE(listedNodes, capturing);
#endif

    timer.start();
    for (int again = 0; again < 3; again++) {
        const QList<CameraDevice> listed = cameraDevices();
        QCOMPARE(listed.size(), first.size());
        for (int i = 0; i < listed.size(); i++) {
            QCOMPARE(listed[i].id, first[i].id);
            QCOMPARE(listed[i].name, first[i].name);
        }
    }
    qInfo().noquote() << "cameras:" << (names.isEmpty() ? QStringLiteral("none") : names.join(QStringLiteral(", "))) << "; listed in" << firstListingMs << "ms the first time and" << timer.elapsed() / 3 << "ms after that";
}

constexpr std::chrono::microseconds CAMERA_INTERVAL(33'333);
constexpr std::chrono::microseconds QUICKEST_READ(4'000);

void TestCameraCapture::aCamerasOwnClockSaysWhenAPictureWasTaken()
{
    using namespace std::chrono;
    constexpr int PICTURES = 300;
    constexpr int READ_VERY_LATE = 150;
    const Clock::time_point began = Clock::time_point(seconds(1000));
    const microseconds cameraBegan = seconds(77);

    CaptureClock clock;
    uint32_t noise = 1;
    std::vector<Clock::time_point> taken;
    qint64 readFurthestApart = 0;
    Clock::time_point lastReadAt;
    for (int index = 0; index < PICTURES; index++) {
        noise = noise * 1664525u + 1013904223u;
        microseconds late((noise >> 8) % 16'000);
        if (index == 0)
            late = microseconds(0);
        if (index == READ_VERY_LATE)
            late = milliseconds(200);
        const Clock::time_point readAt = began + index * CAMERA_INTERVAL + QUICKEST_READ + late;
        if (index > 0)
            readFurthestApart = std::max<qint64>(readFurthestApart, duration_cast<microseconds>(readAt - lastReadAt).count());
        lastReadAt = readAt;

        const Clock::time_point at = clock.at(cameraBegan + index * CAMERA_INTERVAL, readAt);
        QVERIFY(at <= readAt);
        taken.push_back(at);
    }

    QVERIFY(readFurthestApart > 200'000);
    QCOMPARE(taken[0], began + QUICKEST_READ);
    for (size_t i = 1; i < taken.size(); i++) {
        const qint64 apart = duration_cast<microseconds>(taken[i] - taken[i - 1]).count();
        QVERIFY2(std::abs(apart - CAMERA_INTERVAL.count()) <= 1'000, qPrintable(QStringLiteral("pictures %1 and %2 are %3 us apart").arg(i - 1).arg(i).arg(apart)));
    }

    const Clock::time_point later = began + PICTURES * CAMERA_INTERVAL + seconds(1);
    QCOMPARE(clock.at(std::nullopt, later), later);
    QVERIFY(clock.at(std::nullopt, later) > later);
}

void TestCameraCapture::aCameraClockThatRunsSlowIsFollowed()
{
    using namespace std::chrono;
    // twenty minutes of a camera clock losing a ten-thousandth: 120 ms by the end
    constexpr int PICTURES = 36'000;
    const Clock::time_point began = Clock::time_point(seconds(1000));

    CaptureClock clock;
    for (int index = 0; index < PICTURES; index++) {
        const microseconds stamped = index * CAMERA_INTERVAL - index * CAMERA_INTERVAL / 10'000;
        const Clock::time_point readAt = began + index * CAMERA_INTERVAL + QUICKEST_READ;
        const qint64 behind = duration_cast<microseconds>(readAt - clock.at(stamped, readAt)).count();
        QVERIFY2(behind >= 0 && behind <= 1'000, qPrintable(QStringLiteral("picture %1 is taken to be %2 us old").arg(index).arg(behind)));
    }
}

void TestCameraCapture::aCameraClockThatStartsOverIsTakenUpAgain()
{
    using namespace std::chrono;
    constexpr int PICTURES = 90;
    constexpr int STARTS_OVER_AT = 30;
    constexpr int LEAPS_AHEAD_AT = 60;
    const Clock::time_point began = Clock::time_point(seconds(1000));

    CaptureClock clock;
    std::optional<Clock::time_point> last;
    for (int index = 0; index < PICTURES; index++) {
        microseconds stamped = seconds(500) + index * CAMERA_INTERVAL;
        if (index >= STARTS_OVER_AT)
            stamped -= seconds(500);
        if (index >= LEAPS_AHEAD_AT)
            stamped += hours(3);
        const Clock::time_point readAt = began + index * CAMERA_INTERVAL + QUICKEST_READ;
        const Clock::time_point at = clock.at(stamped, readAt);
        QCOMPARE(at, readAt);
        QVERIFY(!last || at > *last);
        last = at;
    }
}

void TestCameraCapture::picturesAreEncodedAtTheSizeAskedFor()
{
    QFETCH(VideoCodec, codec);
    SKIP_A_CODEC_THAT_IS_NOT_ENCODED(codec);
    camera->size = QSize(1280, 720);
    const QSize encodeSize(640, 360);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    const CameraSettings settings = settingsFor(codec, encodeSize, 30);
    pipeline.start(settings);
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 30 || !record.failures.isEmpty(), "fewer than 30 frames in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    pipeline.stop();
    QTRY_COMPARE_WITH_TIMEOUT(camera->openNow.load(), 0, 2000);

    QCOMPARE(camera->timesOpened.load(), 1);
    QCOMPARE(camera->openedWithId, settings.deviceId);
    QCOMPARE(record.starts.size(), 1);
    QCOMPARE(record.starts.first().size, encodeSize);
    QCOMPARE(record.starts.first().framerate, 30);
    QVERIFY(record.frames[0]->keyframe);
    QCOMPARE(record.keyFramesFrom(1), 0);

    const std::vector<int> ticks = record.ticksBetween();
    for (int each : ticks) {
        QVERIFY2(each > 0, qPrintable(QString::number(each)));
        QVERIFY2(std::abs(each - 3000 * static_cast<int>(std::lround(each / 3000.0))) <= 2, qPrintable(QString::number(each)));
    }
    const double fps = framesPerSecond(ticks);
    QVERIFY2(fps > 30 * 0.8 && fps < 30 * 1.05, qPrintable(QString::number(fps)));

    for (const EncodedFramePointer &frame : record.frames)
        QVERIFY(isWellFormed(codec, *frame));
    const Decoded decoded = decodeAll(codec, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.dropped, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(record.frames.size()));
    QVERIFY(arePicturesOfTheSizeSent(codec, decoded.sizes, encodeSize));
}

void TestCameraCapture::aCameraOfAnotherShapeKeepsItsShape_data()
{
    QTest::addColumn<QSize>("cameraSize");
    QTest::addColumn<QSize>("encodeSize");
    QTest::addColumn<QSize>("expected");
    QTest::newRow("4:3 inside 16:9") << QSize(640, 480) << QSize(640, 360) << QSize(480, 360);
    QTest::newRow("16:9 inside 4:3") << QSize(1280, 720) << QSize(640, 480) << QSize(640, 360);
    QTest::newRow("a small camera is not enlarged") << QSize(640, 480) << QSize(1280, 720) << QSize(640, 480);
    QTest::newRow("odd sizes are made even") << QSize(640, 480) << QSize(300, 169) << QSize(224, 168);
}

void TestCameraCapture::aCameraOfAnotherShapeKeepsItsShape()
{
    QFETCH(QSize, cameraSize);
    QFETCH(QSize, encodeSize);
    QFETCH(QSize, expected);
    SKIP_WITHOUT_AN_ENCODER(codec);
    camera->size = cameraSize;
    camera->shows = SyntheticCamera::Shows::Square;

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, encodeSize, 30));
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 10 || !record.failures.isEmpty(), "fewer than 10 frames in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    pipeline.stop();

    QCOMPARE(record.starts.size(), 1);
    QCOMPARE(record.starts.first().size, expected);
    const Decoded decoded = decodeAll(codec, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.sizes, QList<QSize>{ expected });

    QVERIFY(isPlanar420(decoded.last.get()));
    const QSize square = brightExtent(decoded.last.get());
    const int side = cameraSize.height() / 3 * expected.height() / cameraSize.height();
    QVERIFY2(std::abs(square.width() - side) <= 3 && std::abs(square.height() - side) <= 3, qPrintable(QStringLiteral("the square is %1x%2, expected %3 a side").arg(square.width()).arg(square.height()).arg(side)));
}

void TestCameraCapture::picturesAreThinnedToTheFrameRate_data()
{
    QTest::addColumn<int>("cameraRate");
    QTest::addColumn<int>("wanted");
    QTest::addColumn<int>("expected");
    QTest::newRow("30 of 30") << 30 << 30 << 30;
    QTest::newRow("20 of 30") << 30 << 20 << 20;
    QTest::newRow("a camera with fewer gives all it has") << 15 << 30 << 15;
}

void TestCameraCapture::picturesAreThinnedToTheFrameRate()
{
    QFETCH(int, cameraRate);
    QFETCH(int, wanted);
    QFETCH(int, expected);
    SKIP_WITHOUT_AN_ENCODER(codec);
    camera->framerate = cameraRate;

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, QSize(640, 360), wanted));
    const size_t enough = static_cast<size_t>(expected) * 3 / 2;
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= enough || !record.failures.isEmpty(), "too few frames in 6 s", 6000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    pipeline.stop();

    QCOMPARE(record.starts.size(), 1);
    QCOMPARE(record.starts.first().framerate, wanted);

    const std::vector<int> ticks = record.ticksBetween();
    const double fps = framesPerSecond(ticks);
    qInfo() << "frames a second:" << fps;
    QVERIFY2(fps > expected * 0.9 && fps < expected * 1.05, qPrintable(QString::number(fps)));
    const int closest = TICKS_PER_SECOND / expected - TICKS_PER_SECOND / cameraRate;
    for (int each : ticks)
        QVERIFY2(each > closest, qPrintable(QStringLiteral("%1 ticks between two frames").arg(each)));
}

void TestCameraCapture::aNewSizeAndRateStartWithAKeyFrame()
{
    SKIP_WITHOUT_AN_ENCODER(codec);
    const QSize larger(640, 360);
    const QSize smaller(320, 180);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, larger, 30));
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 15 || !record.failures.isEmpty(), "fewer than 15 frames in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));

    const size_t atChange = record.frames.size();
    pipeline.setEncoding(smaller, 15);
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= atChange + 12, 3000);
    const size_t changed = record.keyFrameFrom(atChange);
    QVERIFY(changed < atChange + 4);
    QCOMPARE(record.keyFramesFrom(changed + 1), 0);
    QCOMPARE(record.keyFramesFrom(1), 1);

    const size_t atRepeat = record.frames.size();
    pipeline.setEncoding(smaller, 15);
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= atRepeat + 8, 3000);
    const size_t repeated = record.keyFrameFrom(atRepeat);
    QVERIFY(repeated < atRepeat + 4);
    QCOMPARE(record.keyFramesFrom(repeated + 1), 0);
    pipeline.stop();
    QVERIFY(record.failures.isEmpty());

    QCOMPARE(record.starts, (QList<PipelineRecord::Start>{ { larger, 30 }, { smaller, 15 } }));
    const double fpsAfter = framesPerSecond(record.ticksBetween(changed));
    QVERIFY2(fpsAfter > 15 * 0.9 && fpsAfter < 15 * 1.05, qPrintable(QString::number(fpsAfter)));

    const Decoded before = decodeAll(codec, record.frames, 0, changed);
    QCOMPARE(before.errors, quint64(0));
    QCOMPARE(before.pictures, static_cast<int>(changed));
    QCOMPARE(before.sizes, QList<QSize>{ larger });
    const Decoded after = decodeAll(codec, record.frames, changed);
    QCOMPARE(after.errors, quint64(0));
    QCOMPARE(after.pictures, static_cast<int>(record.frames.size() - changed));
    QCOMPARE(after.sizes, QList<QSize>{ smaller });
}

void TestCameraCapture::aNewBitrateAndSizeTogetherCostOneKeyFrame()
{
    SKIP_WITHOUT_AN_ENCODER(codec);
    constexpr int CHANGES = 3;
    constexpr int LOWER_BITRATE = 300'000;
    const QSize larger(640, 360);
    const QSize smaller(480, 270);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, larger, 30));
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 15 || !record.failures.isEmpty(), "fewer than 15 frames in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));

    std::vector<size_t> changedAt = { 0 };
    QList<PipelineRecord::Start> expectedStarts = { { larger, 30 } };
    for (int change = 0; change < CHANGES; change++) {
        const bool toSmaller = change % 2 == 0;
        const QSize size = toSmaller ? smaller : larger;
        const int bitrate = toSmaller ? LOWER_BITRATE : BITRATE;
        const size_t atChange = record.frames.size();
        const auto newBitrate = [&pipeline, bitrate] { pipeline.setBitrate(bitrate); };
        const auto newSize = [&pipeline, size] { pipeline.setEncoding(size, 30); };
        if (change % 3 == 0) {
            newBitrate();
            newSize();
        } else if (change % 3 == 1) {
            camera->atComingPictures({ newBitrate, newSize });
        } else {
            camera->atComingPictures({ newSize, newBitrate });
        }
        QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= atChange + 20, 3000);
        QVERIFY2(record.keyFramesFrom(atChange) == 1, qPrintable(QStringLiteral("change %1 made %2 key frames").arg(change).arg(record.keyFramesFrom(atChange))));
        changedAt.push_back(record.keyFrameFrom(atChange));
        expectedStarts.append({ size, 30 });
    }

    QTest::qWait(500);
    const size_t measuredFrom = record.frames.size();
    QTest::qWait(1000);
    size_t bytes = 0;
    for (size_t i = measuredFrom; i < record.frames.size(); i++)
        bytes += record.frames[i]->data.size();
    const double bitsPerSecond = static_cast<double>(bytes) * 8.0;
    pipeline.stop();
    QVERIFY(record.failures.isEmpty());
    QVERIFY2(bitsPerSecond > LOWER_BITRATE * 0.5 && bitsPerSecond < LOWER_BITRATE * 1.5, qPrintable(QString::number(bitsPerSecond)));
    QCOMPARE(record.keyFramesFrom(0), CHANGES + 1);
    QCOMPARE(record.starts, expectedStarts);

    changedAt.push_back(record.frames.size());
    for (size_t i = 0; i + 1 < changedAt.size(); i++) {
        const Decoded decoded = decodeAll(codec, record.frames, changedAt[i], changedAt[i + 1]);
        QCOMPARE(decoded.errors, quint64(0));
        QCOMPARE(decoded.pictures, static_cast<int>(changedAt[i + 1] - changedAt[i]));
        QCOMPARE(decoded.sizes, QList<QSize>{ expectedStarts[static_cast<int>(i)].size });
    }
}

void TestCameraCapture::whatIsAskedForBeforeTheFirstFrameShapesIt_data()
{
    QTest::addColumn<int>("askedAfterMs");
    QTest::addColumn<int>("betweenTheTwoMs");
    QTest::newRow("as soon as it is started") << 0 << 0;
    QTest::newRow("one and then the other while the camera is being opened") << 100 << 50;
}

void TestCameraCapture::whatIsAskedForBeforeTheFirstFrameShapesIt()
{
    QFETCH(int, askedAfterMs);
    QFETCH(int, betweenTheTwoMs);
    SKIP_WITHOUT_AN_ENCODER(codec);
    constexpr int LOWER_BITRATE = 300'000;
    const QSize smaller(480, 270);
    camera->opensAfterMs = 400;

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, QSize(640, 360), 30));
    QTest::qWait(askedAfterMs);
    pipeline.setBitrate(LOWER_BITRATE);
    QTest::qWait(betweenTheTwoMs);
    pipeline.setEncoding(smaller, 15);
    QVERIFY(record.frames.empty());
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 38 || !record.failures.isEmpty(), "fewer than 38 frames in 6 s", 6000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    pipeline.stop();

    QVERIFY(record.frames[0]->keyframe);
    QCOMPARE(record.keyFramesFrom(1), 0);
    QCOMPARE(record.starts, (QList<PipelineRecord::Start>{ { smaller, 15 } }));

    const std::vector<int> ticks = record.ticksBetween();
    const double fps = framesPerSecond(ticks);
    QVERIFY2(fps > 15 * 0.9 && fps < 15 * 1.05, qPrintable(QString::number(fps)));
    constexpr size_t SETTLED = 15;
    size_t bytes = 0;
    qint64 ticksMeasured = 0;
    for (size_t i = SETTLED; i < record.frames.size(); i++) {
        bytes += record.frames[i]->data.size();
        ticksMeasured += ticks[i - 1];
    }
    const double bitsPerSecond = static_cast<double>(bytes) * 8.0 * TICKS_PER_SECOND / static_cast<double>(ticksMeasured);
    qInfo() << "bits a second:" << bitsPerSecond;
    QVERIFY2(bitsPerSecond > LOWER_BITRATE * 0.5 && bitsPerSecond < LOWER_BITRATE * 1.5, qPrintable(QString::number(bitsPerSecond)));

    const Decoded decoded = decodeAll(codec, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(record.frames.size()));
    QCOMPARE(decoded.sizes, QList<QSize>{ smaller });
}

void TestCameraCapture::aKeyFrameComesWhenAskedFor()
{
    SKIP_WITHOUT_AN_ENCODER(codec);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, QSize(640, 360), 30));
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 20 || !record.failures.isEmpty(), "fewer than 20 frames in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QVERIFY(record.frames[0]->keyframe);
    QCOMPARE(record.keyFramesFrom(1), 0);

    const size_t atRequest = record.frames.size();
    pipeline.requestKeyframe();
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= atRequest + 10, 2000);
    const size_t forced = record.keyFrameFrom(atRequest);
    QVERIFY(forced < atRequest + 4);
    QCOMPARE(record.keyFramesFrom(forced + 1), 0);
    pipeline.stop();

    const Decoded decoded = decodeAll(codec, record.frames, forced);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(record.frames.size() - forced));
}

void TestCameraCapture::nothingIsEncodedWhileSuspended()
{
    SKIP_WITHOUT_AN_ENCODER(codec);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, QSize(640, 360), 30));
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 15 || !record.failures.isEmpty(), "fewer than 15 frames in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QVERIFY(!record.previews.isEmpty());

    pipeline.setEncodingSuspended(true);
    QTest::qWait(300);
    const size_t whileSuspended = record.frames.size();
    const int previewsBefore = static_cast<int>(record.previews.size());
    const int picturesBefore = camera->picturesGiven;
    pipeline.requestKeyframe();
    QTest::qWait(1000);
    QCOMPARE(record.frames.size(), whileSuspended);
    QCOMPARE(camera->openNow.load(), 1);
    QVERIFY(camera->picturesGiven - picturesBefore >= 20);
    const int previewsMeanwhile = static_cast<int>(record.previews.size()) - previewsBefore;
    QVERIFY2(previewsMeanwhile >= 10 && previewsMeanwhile <= 20, qPrintable(QString::number(previewsMeanwhile)));

    pipeline.setEncodingSuspended(false);
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= whileSuspended + 10, 2000);
    QVERIFY(record.frames[whileSuspended]->keyframe);
    QCOMPARE(record.keyFramesFrom(whileSuspended + 1), 0);
    pipeline.stop();
    QVERIFY(record.failures.isEmpty());
    QCOMPARE(record.starts.size(), 1);

    const Decoded decoded = decodeAll(codec, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(record.frames.size()));
}

void TestCameraCapture::aSecondLayerIsEncodedBesideTheFirstOnceItIsAskedFor()
{
    SKIP_WITHOUT_AN_ENCODER(codec);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.setEncoding(QSize(320, 180), 15, 1);
    pipeline.setBitrate(150000, 1);
    pipeline.start(settingsFor(codec, QSize(640, 360), 30));
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 15 || !record.failures.isEmpty(), "fewer than 15 frames in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QVERIFY(record.secondFrames.empty());
    QVERIFY(record.secondStarts.isEmpty());

    const size_t firstLayerBefore = record.frames.size();
    pipeline.setEncodingSuspended(false, 1);
    QTRY_VERIFY2_WITH_TIMEOUT(record.secondFrames.size() >= 10 || !record.failures.isEmpty(), "fewer than 10 frames of the second layer in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QVERIFY(record.secondFrames[0]->keyframe);
    QCOMPARE(record.secondStarts, QList<PipelineRecord::Start>({ { QSize(320, 180), 15 } }));
    QVERIFY(record.frames.size() > firstLayerBefore + 10);
    QCOMPARE(record.keyFramesFrom(firstLayerBefore), 0);
    QCOMPARE(record.starts.size(), 1);

    const size_t secondLayerBefore = record.secondFrames.size();
    const size_t firstLayerThen = record.frames.size();
    pipeline.requestKeyframe(1);
    const auto keyFrameSince = [&record, secondLayerBefore] { return std::any_of(record.secondFrames.begin() + static_cast<std::ptrdiff_t>(secondLayerBefore), record.secondFrames.end(), [](const EncodedFramePointer &frame) { return frame->keyframe; }); };
    QTRY_VERIFY_WITH_TIMEOUT(keyFrameSince(), 2000);
    QCOMPARE(record.keyFramesFrom(firstLayerThen), 0);

    pipeline.setEncodingSuspended(true, 1);
    QTest::qWait(300);
    const size_t secondLayerStopped = record.secondFrames.size();
    const size_t firstLayerMeanwhile = record.frames.size();
    QTest::qWait(500);
    QCOMPARE(record.secondFrames.size(), secondLayerStopped);
    QVERIFY(record.frames.size() > firstLayerMeanwhile + 5);
    pipeline.stop();
    QVERIFY(record.failures.isEmpty());

    const Decoded first = decodeAll(codec, record.frames);
    QCOMPARE(first.errors, quint64(0));
    QCOMPARE(first.sizes, QList<QSize>{ QSize(640, 360) });
    const Decoded second = decodeAll(codec, record.secondFrames);
    QCOMPARE(second.errors, quint64(0));
    QCOMPARE(second.pictures, static_cast<int>(record.secondFrames.size()));
    QCOMPARE(second.sizes, QList<QSize>{ QSize(320, 180) });
}

void TestCameraCapture::aCameraOpenedWhileSuspendedIsAnnounced()
{
    SKIP_WITHOUT_AN_ENCODER(codec);
    camera->size = QSize(640, 480);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.setEncodingSuspended(true);
    pipeline.start(settingsFor(codec, QSize(640, 360), 30));
    QTRY_VERIFY2_WITH_TIMEOUT(!record.starts.isEmpty() || !record.failures.isEmpty(), "not started in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(record.starts.first().size, QSize(480, 360));
    QTRY_VERIFY_WITH_TIMEOUT(record.previews.size() >= 5, 2000);
    QVERIFY(record.frames.empty());

    pipeline.setEncodingSuspended(false);
    QTRY_VERIFY_WITH_TIMEOUT(record.frames.size() >= 5, 3000);
    QVERIFY(record.frames[0]->keyframe);
    QCOMPARE(record.starts.size(), 1);
    pipeline.stop();

    const Decoded decoded = decodeAll(codec, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.sizes, QList<QSize>{ QSize(480, 360) });
}

// MJPEG cameras send full range; passed on as is, a viewer assuming video range would crush darks and
// lights
void TestCameraCapture::aFullRangeCameraKeepsItsBrightness()
{
    QFETCH(VideoCodec, codec);
    SKIP_A_CODEC_THAT_IS_NOT_ENCODED(codec);
    constexpr int WITHIN = 4;
    camera->format = AV_PIX_FMT_YUVJ422P;
    camera->fullRange = true;
    camera->shows = SyntheticCamera::Shows::TwoGreys;

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, QSize(640, 360), 30));
    QTRY_VERIFY2_WITH_TIMEOUT((record.frames.size() >= 10 && !record.previews.isEmpty()) || !record.failures.isEmpty(), "fewer than 10 frames in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    pipeline.stop();

    const Decoded decoded = decodeAll(codec, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QVERIFY(arePicturesOfTheSizeSent(codec, decoded.sizes, QSize(640, 360)));
    const QImage sent = inRgb(decoded.last.get());
    QVERIFY(!sent.isNull());
    const QRgb dark = sent.pixel(sent.width() / 4, sent.height() / 2);
    const QRgb light = sent.pixel(sent.width() * 3 / 4, sent.height() / 2);
    qInfo().noquote() << "greys of" << DARK_GREY << "and" << LIGHT_GREY << "are sent as" << describe(dark) << "and" << describe(light);
    QVERIFY2(isGrey(dark, DARK_GREY, WITHIN), qPrintable(describe(dark)));
    QVERIFY2(isGrey(light, LIGHT_GREY, WITHIN), qPrintable(describe(light)));

    const QImage preview = record.previews.last();
    const QRgb previewDark = preview.pixel(preview.width() / 4, preview.height() / 2);
    const QRgb previewLight = preview.pixel(preview.width() * 3 / 4, preview.height() / 2);
    QVERIFY2(isGrey(previewDark, DARK_GREY, WITHIN), qPrintable(describe(previewDark)));
    QVERIFY2(isGrey(previewLight, LIGHT_GREY, WITHIN), qPrintable(describe(previewLight)));
}

void TestCameraCapture::aCamerasColoursAreWhatAViewerTakesThemFor_data()
{
    QTest::addColumn<VideoCodec>("codec");
    QTest::addColumn<AVColorSpace>("cameraColorspace");
    QTest::addColumn<QSize>("size");
    const QList<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    for (VideoCodec codec : encodable) {
        for (const QSize &size : { QSize(1280, 720), QSize(640, 360) }) {
            const QString sent = QStringLiteral("%1 at %2x%3 from a ").arg(videoCodecName(codec)).arg(size.width()).arg(size.height());
            QTest::newRow(qPrintable(sent + QStringLiteral("BT.601 camera"))) << codec << AVCOL_SPC_SMPTE170M << size;
            QTest::newRow(qPrintable(sent + QStringLiteral("BT.709 camera"))) << codec << AVCOL_SPC_BT709 << size;
        }
    }
    if (encodable.isEmpty())
        QTest::newRow("no encoder") << VideoCodec::Unknown << AVCOL_SPC_UNSPECIFIED << QSize();
}

// untagged streams are taken as BT.709 from 720 lines up and BT.601 below; VP8 as BT.601 always
void TestCameraCapture::aCamerasColoursAreWhatAViewerTakesThemFor()
{
    QFETCH(VideoCodec, codec);
    QFETCH(AVColorSpace, cameraColorspace);
    QFETCH(QSize, size);
    SKIP_A_CODEC_THAT_IS_NOT_ENCODED(codec);
    camera->size = QSize(1280, 720);
    camera->format = AV_PIX_FMT_YUVJ422P;
    camera->fullRange = true;
    camera->colorspace = cameraColorspace;
    camera->shows = SyntheticCamera::Shows::OneColour;

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, size, 30));
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 10 || !record.failures.isEmpty(), "fewer than 10 frames in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    pipeline.stop();

    const Decoded decoded = decodeAll(codec, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QVERIFY(arePicturesOfTheSizeSent(codec, decoded.sizes, size));
    QVERIFY(isPlanar420(decoded.last.get()));

    const bool sentAsBt709 = codec != VideoCodec::VP8 && size.height() >= 720;
    const Yuv middle = middleOf(decoded.last.get());
    const Yuv right = inYuv(COLOUR_RED, COLOUR_GREEN, COLOUR_BLUE, sentAsBt709, false);
    const Yuv wrong = inYuv(COLOUR_RED, COLOUR_GREEN, COLOUR_BLUE, !sentAsBt709, false);
    qInfo() << "the colour decodes to" << middle.y << middle.u << middle.v << ", off what it should be by" << distance(middle, right) << "and off the other matrix by" << distance(middle, wrong) << "; the stream names matrix" << decoded.last->colorspace << "and range" << decoded.last->color_range;
    QVERIFY(distance(middle, right) < 6);
    QVERIFY(distance(middle, wrong) > 12);
    QVERIFY(decoded.last->color_range != AVCOL_RANGE_JPEG);
    if (codec != VideoCodec::VP8) {
        QCOMPARE(Core::Media::usesBt709(decoded.last.get()), sentAsBt709);
        const QRgb shown = inRgb(decoded.last.get()).pixel(size.width() / 2, size.height() / 2);
        QVERIFY2(std::abs(qRed(shown) - COLOUR_RED) <= 6 && std::abs(qGreen(shown) - COLOUR_GREEN) <= 6 && std::abs(qBlue(shown) - COLOUR_BLUE) <= 6, qPrintable(describe(shown)));
    }
}

void TestCameraCapture::thePreviewIsSmallAndFrequent_data()
{
    QTest::addColumn<QSize>("cameraSize");
    QTest::addColumn<QSize>("expected");
    QTest::newRow("1280x720") << QSize(1280, 720) << QSize(640, 360);
    QTest::newRow("4:3") << QSize(640, 480) << QSize(480, 360);
    QTest::newRow("a small camera is not enlarged") << QSize(320, 240) << QSize(320, 240);
}

void TestCameraCapture::thePreviewIsSmallAndFrequent()
{
    QFETCH(QSize, cameraSize);
    QFETCH(QSize, expected);
    SKIP_WITHOUT_AN_ENCODER(codec);
    camera->size = cameraSize;
    camera->shows = SyntheticCamera::Shows::Square;

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, QSize(320, 180), 30));
    QTRY_VERIFY2_WITH_TIMEOUT(!record.previews.isEmpty() || !record.failures.isEmpty(), "no preview in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    if (cameraSize == QSize(1280, 720)) {
        const int before = static_cast<int>(record.previews.size());
        QTest::qWait(2000);
        const int inTwoSeconds = static_cast<int>(record.previews.size()) - before;
        QVERIFY2(inTwoSeconds >= 24 && inTwoSeconds <= 34, qPrintable(QString::number(inTwoSeconds)));
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(record.previews.size() >= 2, 2000);
    }
    pipeline.stop();

    for (const QImage &preview : record.previews) {
        QVERIFY(!preview.isNull());
        QVERIFY(preview.width() <= 640 && preview.height() <= 360);
        QCOMPARE(preview.size(), expected);
    }
    QVERIFY(record.previews.first().constBits() != record.previews.last().constBits());

    const QImage last = record.previews.last();
    int across = 0;
    int down = 0;
    for (int x = 0; x < last.width(); x++)
        across += qGreen(last.pixel(x, last.height() / 2)) > 128 ? 1 : 0;
    for (int y = 0; y < last.height(); y++)
        down += qGreen(last.pixel(last.width() / 2, y)) > 128 ? 1 : 0;
    const int side = cameraSize.height() / 3 * expected.height() / cameraSize.height();
    QVERIFY2(std::abs(across - side) <= 3 && std::abs(down - side) <= 3, qPrintable(QStringLiteral("the square is %1x%2, expected %3 a side").arg(across).arg(down).arg(side)));
}

void TestCameraCapture::theBitrateIsFollowed()
{
    SKIP_WITHOUT_AN_ENCODER(codec);
    constexpr int LOWER_BITRATE = 300'000;
    constexpr int SETTLING_MS = 600;
    constexpr int MEASURED_MS = 1000;

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    CameraSettings settings = settingsFor(codec, QSize(640, 360), 30);
    settings.bitrateBps = 9'000'000;
    settings.maxBitrateBps = BITRATE;
    pipeline.start(settings);
    QTRY_VERIFY2_WITH_TIMEOUT(!record.frames.empty() || !record.failures.isEmpty(), "no frame in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));

    const auto bitsPerSecond = [&record] {
        QTest::qWait(SETTLING_MS);
        const size_t from = record.frames.size();
        QTest::qWait(MEASURED_MS);
        size_t bytes = 0;
        for (size_t i = from; i < record.frames.size(); i++)
            bytes += record.frames[i]->data.size();
        return static_cast<double>(bytes) * 8.0 * 1000.0 / MEASURED_MS;
    };
    const double before = bitsPerSecond();
    pipeline.setBitrate(LOWER_BITRATE);
    const double after = bitsPerSecond();
    pipeline.stop();
    qInfo() << "bits a second at" << BITRATE << ":" << before << ", at" << LOWER_BITRATE << ":" << after;
    QVERIFY2(before > BITRATE * 0.5 && before < BITRATE * 1.4, qPrintable(QString::number(before)));
    QVERIFY2(after > LOWER_BITRATE * 0.5 && after < LOWER_BITRATE * 1.5, qPrintable(QString::number(after)));
    QVERIFY(record.failures.isEmpty());

    const Decoded decoded = decodeAll(codec, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
}

void TestCameraCapture::statisticsComeEverySecond()
{
    SKIP_WITHOUT_AN_ENCODER(codec);
    camera->size = QSize(640, 480);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, QSize(640, 360), 15));
    QTRY_VERIFY2_WITH_TIMEOUT(record.stats.size() >= 3 || !record.failures.isEmpty(), "fewer than 3 statistics in 6 s", 6000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    pipeline.stop();

    for (int i = 1; i < 3; i++) {
        const qint64 apart = record.statsAt[i] - record.statsAt[i - 1];
        QVERIFY2(apart > 800 && apart < 1300, qPrintable(QString::number(apart)));
    }
    const PipelineStats last = record.stats[2];
    qInfo().noquote() << QStringLiteral("statistics: %1, hardware %2, %3x%4, captured %5 fps, encoded %6 fps, %7 ms in the encoder a picture, %8 bits a second, %9 frames").arg(last.encoderName).arg(last.hardware).arg(last.encodedSize.width()).arg(last.encodedSize.height()).arg(last.capturedFps, 0, 'f', 1).arg(last.encodedFps, 0, 'f', 1).arg(last.averageEncodeMs, 0, 'f', 2).arg(last.bitrateBps).arg(last.framesEncoded);
    QVERIFY(last.encoderName.endsWith(QStringLiteral(", from a synthetic camera")));
    QVERIFY(!last.encoderName.startsWith(QStringLiteral("no encoder yet")));
    QCOMPARE(last.encodedSize, QSize(480, 360));
    QVERIFY2(last.capturedFps > 30 * 0.85 && last.capturedFps < 30 * 1.15, qPrintable(QString::number(last.capturedFps)));
    QVERIFY2(last.encodedFps > 15 * 0.85 && last.encodedFps < 15 * 1.15, qPrintable(QString::number(last.encodedFps)));
    QVERIFY(last.bitrateBps > 0);
    QVERIFY(last.averageEncodeMs > 0);
    QVERIFY(last.framesEncoded >= 30);
    QVERIFY(last.framesEncoded <= static_cast<quint64>(record.frames.size()));
}

void TestCameraCapture::aCameraThatIsLostFails()
{
    SKIP_WITHOUT_AN_ENCODER(codec);
    camera->lostAfter = 12;

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, QSize(640, 360), 30));
    QTRY_COMPARE_WITH_TIMEOUT(record.failures.size(), 1, 5000);
    QCOMPARE(record.failures.first(), QStringLiteral("the synthetic camera was lost"));
    QTRY_COMPARE_WITH_TIMEOUT(camera->openNow.load(), 0, 2000);

    const size_t frames = record.frames.size();
    QVERIFY(frames >= 1 && frames <= 12);
    QTest::qWait(500);
    QCOMPARE(record.frames.size(), frames);
    QCOMPARE(record.failures.size(), 1);
    QCOMPARE(camera->timesOpened.load(), 1);

    const Decoded decoded = decodeAll(codec, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(frames));
}

void TestCameraCapture::aCameraThatDoesNotOpenFails()
{
    SKIP_WITHOUT_AN_ENCODER(codec);
    camera->opens = false;

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, QSize(640, 360), 30));
    QTRY_COMPARE_WITH_TIMEOUT(record.failures.size(), 1, 5000);
    QCOMPARE(record.failures.first(), QStringLiteral("the synthetic camera does not open"));
    QTest::qWait(300);
    QVERIFY(record.frames.empty());
    QVERIFY(record.starts.isEmpty());
    QVERIFY(record.previews.isEmpty());
    QCOMPARE(camera->timesOpened.load(), 1);
}

void TestCameraCapture::noCameraIsOpenedWithoutAnEncoder()
{
    VideoCodec codec = VideoCodec::Unknown;
    for (VideoCodec each : { VideoCodec::H265, VideoCodec::AV1 }) {
        if (!Core::Media::encodableVideoCodecs().contains(each))
            codec = each;
    }

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, QSize(640, 360), 30));
    QTRY_COMPARE_WITH_TIMEOUT(record.failures.size(), 1, 5000);
    qInfo() << "failed with:" << record.failures.first();
    QVERIFY(record.failures.first().contains(videoCodecName(codec)));
    QTest::qWait(300);
    QCOMPARE(camera->timesOpened.load(), 0);
    QVERIFY(record.frames.empty());
    QVERIFY(record.starts.isEmpty());
}

void TestCameraCapture::stoppingClosesTheCameraAndItOpensAgain()
{
    SKIP_WITHOUT_AN_ENCODER(codec);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    const CameraSettings settings = settingsFor(codec, QSize(640, 360), 30);
    pipeline.start(settings);
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 10 || !record.failures.isEmpty(), "fewer than 10 frames in 5 s", 5000);
    QVERIFY2(record.failures.isEmpty(), qPrintable(record.failures.join(QLatin1Char(';'))));
    QCOMPARE(camera->openNow.load(), 1);

    pipeline.stop();
    QTRY_COMPARE_WITH_TIMEOUT(camera->openNow.load(), 0, 2000);
    QTest::qWait(100);
    const size_t afterStop = record.frames.size();
    const int previewsAfterStop = static_cast<int>(record.previews.size());
    QTest::qWait(400);
    QCOMPARE(record.frames.size(), afterStop);
    QCOMPARE(static_cast<int>(record.previews.size()), previewsAfterStop);

    const uint32_t lastTimestamp = record.frames.back()->rtpTimestamp;
    record.frames.clear();
    pipeline.start(settings);
    QTRY_VERIFY2_WITH_TIMEOUT(record.frames.size() >= 5 || !record.failures.isEmpty(), "no frames after opening again", 5000);
    QVERIFY(record.failures.isEmpty());
    QCOMPARE(camera->timesOpened.load(), 2);
    QCOMPARE(record.starts.size(), 2);
    QVERIFY(record.frames[0]->keyframe);
    QVERIFY(static_cast<int32_t>(record.frames[0]->rtpTimestamp - lastTimestamp) > 0);

    record.frames.clear();
    pipeline.start(settings);
    QTRY_COMPARE_WITH_TIMEOUT(camera->timesOpened.load(), 3, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(camera->openNow.load(), 1, 2000);
    QVERIFY(record.failures.isEmpty());
}

void TestCameraCapture::theFirstCameraDeliversPictures()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    setCameraFrameSourceFactory({});
    if (!cameraCaptureAvailable())
        QSKIP("this build cannot open cameras");
    const QList<CameraDevice> cameras = cameraDevices();
    if (cameras.isEmpty())
        QSKIP("no camera is connected");
    SKIP_WITHOUT_AN_ENCODER(codec);
    const QSize encodeSize(640, 360);

    auto pipeline = std::make_unique<CameraPipeline>();
    PipelineRecord record(*pipeline);
    CameraSettings settings = settingsFor(codec, encodeSize, 30);
    settings.deviceId.clear();

    static std::mutex saidMutex;
    static QStringList said;
    static QtMessageHandler handler;
    handler = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context, const QString &message) {
        {
            std::lock_guard lock(saidMutex);
            said.append(message);
        }
        handler(type, context, message);
    });

    QElapsedTimer open;
    open.start();
    pipeline->start(settings);
    double cpuAtFirstFrame = 0;
    size_t framesAtCpuStart = 0;
    while (open.elapsed() < REAL_CAMERA_MS && record.failures.isEmpty()) {
        QTest::qWait(5);
        if (cpuAtFirstFrame == 0 && !record.frames.empty()) {
            cpuAtFirstFrame = processCpuMs();
            framesAtCpuStart = record.frames.size();
        }
    }
    const double cpuAtStop = processCpuMs();
    const size_t framesAtStop = record.frames.size();
    pipeline->stop();
    pipeline.reset();
    qInstallMessageHandler(handler);
    qInfo() << "from starting to open the camera to having closed it:" << open.elapsed() << "ms";
    QCoreApplication::processEvents();

    if (!record.failures.isEmpty())
        QSKIP(qPrintable(QStringLiteral("the camera could not be used: %1").arg(record.failures.first())));
    if (record.frames.empty() && record.previews.isEmpty())
        QSKIP("the first camera opened and sent no picture in the two seconds it may be open for");
    QVERIFY(!record.frames.empty());

    const QStringList opened = said.filter(QStringLiteral("opened as"));
    QCOMPARE(opened.size(), 1);
    QVERIFY2(opened.first().contains(QRegularExpression(QStringLiteral("opened as \\S+ \\S+ (\\d+x\\d+) at .* when asked for (\\1|nothing)$"))), qPrintable(opened.first()));

    const std::vector<int> ticks = record.ticksBetween();
    for (int each : ticks)
        QVERIFY(each > 0);
    const double cpuPerFrame = framesAtStop > framesAtCpuStart ? (cpuAtStop - cpuAtFirstFrame) / static_cast<double>(framesAtStop - framesAtCpuStart) : 0;
    const auto [soonest, latest] = std::minmax_element(ticks.begin(), ticks.end());
    qInfo().noquote() << QStringLiteral("the first frame came %1 ms after the start; %2 frames at %3 a second, between %4 and %5 ticks apart, %6 previews, %7 ms of processor time a frame").arg(record.firstFrameAt).arg(record.frames.size()).arg(framesPerSecond(ticks), 0, 'f', 1).arg(ticks.empty() ? 0 : *soonest).arg(ticks.empty() ? 0 : *latest).arg(record.previews.size()).arg(cpuPerFrame, 0, 'f', 2);
    if (!record.stats.isEmpty())
        qInfo().noquote() << "statistics:" << record.stats.last().encoderName << ", captured" << record.stats.last().capturedFps << "fps";

    QCOMPARE(record.starts.size(), 1);
    const QSize sent = record.starts.first().size;
    QVERIFY(sent.width() <= encodeSize.width() && sent.height() <= encodeSize.height());
    QVERIFY(sent.width() % 2 == 0 && sent.height() % 2 == 0);
    QVERIFY(record.frames[0]->keyframe);
    for (const EncodedFramePointer &frame : record.frames)
        QVERIFY(isWellFormed(codec, *frame));

    QVERIFY(!record.previews.isEmpty());
    for (const QImage &preview : record.previews) {
        QVERIFY(!preview.isNull());
        QVERIFY(preview.width() <= 640 && preview.height() <= 360);
    }

    const Decoded decoded = decodeAll(codec, record.frames);
    QCOMPARE(decoded.errors, quint64(0));
    QCOMPARE(decoded.pictures, static_cast<int>(record.frames.size()));
    QCOMPARE(decoded.sizes, QList<QSize>{ sent });
}

// a v4l2 picture is in the driver's memory, gone once the camera closes
void TestCameraCapture::aPictureOutlivesItsCamera()
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    setCameraFrameSourceFactory({});
    std::unique_ptr<CameraFrameSource> source = makeCameraFrameSource();
    if (!source)
        QSKIP("this build cannot open cameras");
    if (cameraDevices().isEmpty())
        QSKIP("no camera is connected");
    if (!source->open(QString()))
        QSKIP(qPrintable(QStringLiteral("the camera could not be used: %1").arg(source->failure())));

    const FramePointer picture = makeFrame();
    Clock::time_point capturedAt;
    CameraFrameSource::Status status = CameraFrameSource::Status::NoFrame;
    QElapsedTimer open;
    open.start();
    while (status == CameraFrameSource::Status::NoFrame && open.elapsed() < REAL_CAMERA_MS)
        status = source->read(picture.get(), capturedAt);
    source.reset();
    if (status != CameraFrameSource::Status::Frame)
        QSKIP("the first camera sent no picture in the two seconds it may be open for");

    QVERIFY(picture->colorspace != AVCOL_SPC_UNSPECIFIED);
    QVERIFY(!inRgb(picture.get()).isNull());
}

QTEST_GUILESS_MAIN(TestCameraCapture)
#include "tst_CameraCapture.moc"
