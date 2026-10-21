#include "Core/Video/CameraCapture.hpp"

#include "Core/Logging.hpp"
#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Video/CameraPipeline.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#if defined(ACHERON_HAVE_AVDEVICE) && (defined(Q_OS_WIN) || defined(Q_OS_LINUX))
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_desc.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}
#endif

#if defined(ACHERON_HAVE_AVDEVICE) && defined(Q_OS_LINUX)
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace Acheron {
namespace Core {
namespace Video {

namespace {

std::mutex factoryMutex;
CameraFrameSourceFactory replacementFactory;

#if defined(ACHERON_HAVE_AVDEVICE) && (defined(Q_OS_WIN) || defined(Q_OS_LINUX))

using Clock = std::chrono::steady_clock;

QString cameraText(const char *text)
{
    return QCoreApplication::translate("Acheron::Core::Video::CameraPipeline", text);
}

#ifdef Q_OS_WIN
constexpr const char *DEVICE = "dshow";
constexpr const char *URL_BEFORE_ID = "video=";
#else
constexpr const char *DEVICE = "v4l2";
constexpr const char *URL_BEFORE_ID = "";
#endif

struct Mode
{
    const char *size;
    const char *framerate;
    AVCodecID codec;
};

constexpr std::array MODES = {
    Mode{ "1280x720", "30", AV_CODEC_ID_MJPEG },
    Mode{ "1280x720", "30", AV_CODEC_ID_NONE },
    Mode{ "640x480", "30", AV_CODEC_ID_NONE },
    Mode{ nullptr, nullptr, AV_CODEC_ID_NONE },
};

// a full device buffer drops the newest picture, not the oldest, so it is kept small and read at once
constexpr int DEVICE_BUFFER_BYTES = 2 * 1280 * 720 * 2;

// ffmpeg's blocking DirectShow read cant be woken, and a camera gone quiet would hold its reader for
// good, so it is polled
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(1);

using Media::FramePointer;
using Media::InputPointer;
using Media::PacketPointer;

struct OpenCamera
{
    InputPointer input;
    Media::CodecContextPointer decoder;
    const char *sizeAskedFor = nullptr;
};

const AVInputFormat *inputDevice()
{
    static const AVInputFormat *const device = [] {
        avdevice_register_all();
        return av_find_input_format(DEVICE);
    }();
    return device;
}

AVFormatContext *openInput(const QByteArray &url, const Mode &mode)
{
    AVFormatContext *input = avformat_alloc_context();
    if (!input)
        return nullptr;
    // the device has no option for a compressed format, only this
    input->video_codec_id = mode.codec;
    input->flags |= AVFMT_FLAG_NONBLOCK;

    AVDictionary *options = nullptr;
    if (mode.size) {
        av_dict_set(&options, "video_size", mode.size, 0);
        av_dict_set(&options, "framerate", mode.framerate, 0);
    }
    av_dict_set_int(&options, "rtbufsize", DEVICE_BUFFER_BYTES, 0);
    // frees the context when it fails
    avformat_open_input(&input, url.constData(), inputDevice(), &options);
    av_dict_free(&options);
    return input;
}

AVCodecContext *openDecoder(const AVStream *stream)
{
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext *decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!decoder)
        return nullptr;
    // threaded decoding returns each picture that many pictures late
    decoder->thread_count = 1;
    decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_parameters_to_context(decoder, stream->codecpar) < 0 || avcodec_open2(decoder, codec, nullptr) < 0)
        avcodec_free_context(&decoder);
    return decoder;
}

OpenCamera openCamera(const QByteArray &url)
{
    for (const Mode &mode : MODES) {
        OpenCamera camera;
        camera.input.reset(openInput(url, mode));
        if (!camera.input || camera.input->nb_streams == 0 || camera.input->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
#ifdef Q_OS_LINUX
        // v4l2 opens at the nearest size the camera has, where dshow refuses
        const AVCodecParameters *opened = camera.input->streams[0]->codecpar;
        if (mode.size && QStringLiteral("%1x%2").arg(opened->width).arg(opened->height) != QLatin1String(mode.size))
            continue;
#endif
        camera.sizeAskedFor = mode.size;
        camera.decoder.reset(openDecoder(camera.input->streams[0]));
        if (camera.decoder)
            return camera;
    }
    return {};
}

QString describe(const AVStream *stream)
{
    const AVCodecParameters *sent = stream->codecpar;
    const char *format = sent->codec_id == AV_CODEC_ID_RAWVIDEO ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(sent->format)) : avcodec_get_name(sent->codec_id);
    return QStringLiteral("%1 %2 %3x%4 at %5 fps").arg(QString::fromLatin1(DEVICE), QString::fromLatin1(format ? format : "unknown")).arg(sent->width).arg(sent->height).arg(av_q2d(stream->avg_frame_rate), 0, 'g', 4);
}

#ifdef Q_OS_LINUX
// a v4l2 packet is the driver's mapped memory, unmapped when the camera closes
bool copyOutOfTheDriver(AVPacket *packet)
{
    AVBufferRef *copy = av_buffer_alloc(packet->size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!copy)
        return false;
    std::copy_n(packet->data, packet->size, copy->data);
    std::fill_n(copy->data + packet->size, AV_INPUT_BUFFER_PADDING_SIZE, 0);
    av_buffer_unref(&packet->buf);
    packet->buf = copy;
    packet->data = copy->data;
    return true;
}
#endif

#endif

} // namespace

std::chrono::steady_clock::time_point CaptureClock::at(std::optional<std::chrono::microseconds> stamped, std::chrono::steady_clock::time_point readAt)
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point taken = readAt;
    if (stamped) {
        const Clock::duration ahead = readAt.time_since_epoch() - *stamped;
        const Clock::duration followed = ourClockAhead ? *ourClockAhead + (readAt - lastReadAt) / SLOWEST_CLOCK_FOLLOWED : ahead;
        ourClockAhead = ahead - followed > STARTED_OVER ? ahead : std::min(ahead, followed);
        taken = Clock::time_point(std::chrono::duration_cast<Clock::duration>(*stamped + *ourClockAhead));
    }
    lastReadAt = readAt;
    last = last ? std::max(*last + std::chrono::microseconds(1), taken) : taken;
    return *last;
}

void setCameraFrameSourceFactory(CameraFrameSourceFactory factory)
{
    std::lock_guard lock(factoryMutex);
    replacementFactory = std::move(factory);
}

std::unique_ptr<CameraFrameSource> makeCameraFrameSource()
{
    {
        std::lock_guard lock(factoryMutex);
        if (replacementFactory)
            return replacementFactory();
    }
#if defined(ACHERON_HAVE_AVDEVICE) && (defined(Q_OS_WIN) || defined(Q_OS_LINUX))
    return std::make_unique<CameraCapture>();
#else
    return nullptr;
#endif
}

#if defined(ACHERON_HAVE_AVDEVICE) && (defined(Q_OS_WIN) || defined(Q_OS_LINUX))

bool cameraCaptureAvailable()
{
    return inputDevice() != nullptr;
}

#ifdef Q_OS_WIN

QList<CameraDevice> cameraDevices()
{
    AVDeviceInfoList *found = nullptr;
    if (!inputDevice() || avdevice_list_input_sources(inputDevice(), nullptr, nullptr, &found) < 0)
        return {};

    // microphones are in the same list
    QList<CameraDevice> cameras;
    for (int i = 0; i < found->nb_devices; i++) {
        const AVDeviceInfo *device = found->devices[i];
        const AVMediaType *const types = device->media_types;
        if (std::find(types, types + device->nb_media_types, AVMEDIA_TYPE_VIDEO) != types + device->nb_media_types)
            cameras.append({ QString::fromUtf8(device->device_name), QString::fromUtf8(device->device_description) });
    }
    avdevice_free_list_devices(&found);
    return cameras;
}

#else

QList<CameraDevice> cameraDevices()
{
    if (!inputDevice())
        return {};

    // udev's name for a USB camera survives its /dev/videoN changing
    QHash<QString, QString> lasting;
    const QFileInfoList links = QDir(QStringLiteral("/dev/v4l/by-id")).entryInfoList(QDir::System);
    for (const QFileInfo &link : links)
        lasting.insert(link.canonicalFilePath(), link.filePath());

    QList<CameraDevice> cameras;
    const QStringList nodes = QDir(QStringLiteral("/dev")).entryList({ QStringLiteral("video*") }, QDir::System, QDir::Name);
    for (const QString &node : nodes) {
        const QString path = QStringLiteral("/dev/") + node;
        v4l2_capability device{};
        const int fd = open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        const bool answered = ioctl(fd, VIDIOC_QUERYCAP, &device) == 0;
        close(fd);
        // `capabilities` covers all of a camera's nodes, metadata node included
        const __u32 own = device.capabilities & V4L2_CAP_DEVICE_CAPS ? device.device_caps : device.capabilities;
        if (answered && (own & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) && (own & V4L2_CAP_STREAMING))
            cameras.append({ lasting.value(path, path), QString::fromUtf8(reinterpret_cast<const char *>(device.card)) });
    }
    return cameras;
}

#endif

CameraCapture::CameraCapture() : newest(av_frame_alloc()) {}

CameraCapture::~CameraCapture()
{
    stopping = true;
    if (thread.joinable())
        thread.join();
    av_frame_free(&newest);
}

bool CameraCapture::open(const QString &deviceId)
{
    // ffmpeg enters a COM apartment on open and leaves it on close, so one thread must do both
    thread = std::thread([this, deviceId] { run(deviceId); });

    std::unique_lock lock(mutex);
    changed.wait(lock, [this] { return state != State::Opening; });
    return state == State::Open;
}

CameraFrameSource::Status CameraCapture::read(AVFrame *into, std::chrono::steady_clock::time_point &capturedAt)
{
    std::unique_lock lock(mutex);
    changed.wait_for(lock, std::chrono::milliseconds(READ_WAIT_MS), [this] { return newest->buf[0] || state == State::Ended; });
    if (newest->buf[0]) {
        av_frame_move_ref(into, newest);
        capturedAt = newestCapturedAt;
        return Status::Frame;
    }
    return state == State::Ended ? Status::Ended : Status::NoFrame;
}

QString CameraCapture::failure() const
{
    std::lock_guard lock(mutex);
    return failureText;
}

QString CameraCapture::description() const
{
    std::lock_guard lock(mutex);
    return mode;
}

void CameraCapture::opened(const QString &openedMode)
{
    {
        std::lock_guard lock(mutex);
        mode = openedMode;
        state = State::Open;
    }
    changed.notify_all();
}

void CameraCapture::end(const QString &reason)
{
    {
        std::lock_guard lock(mutex);
        failureText = reason;
        state = State::Ended;
    }
    changed.notify_all();
}

void CameraCapture::publish(AVFrame *picture, std::chrono::steady_clock::time_point capturedAt)
{
    // the USB video class default for a camera that names none
    if (picture->colorspace == AVCOL_SPC_UNSPECIFIED)
        picture->colorspace = AVCOL_SPC_SMPTE170M;
    {
        std::lock_guard lock(mutex);
        av_frame_unref(newest);
        av_frame_move_ref(newest, picture);
        newestCapturedAt = capturedAt;
    }
    changed.notify_all();
}

void CameraCapture::run(const QString &deviceId)
{
    const QList<CameraDevice> cameras = cameraDevices();
    if (deviceId.isEmpty() && cameras.isEmpty()) {
        end(cameraText("No camera is connected."));
        return;
    }
    const QString id = deviceId.isEmpty() ? cameras.first().id : deviceId;
    const auto listed = std::find_if(cameras.begin(), cameras.end(), [&id](const CameraDevice &each) { return each.id == id; });

    const OpenCamera camera = openCamera(URL_BEFORE_ID + id.toUtf8());
    if (!camera.input) {
        end(listed == cameras.end() ? cameraText("The camera is not connected.") : cameraText("The camera could not be opened. Another program may be using it."));
        return;
    }
    const AVStream *stream = camera.input->streams[0];
    const QString openedMode = describe(stream);
    qCInfo(LogVideo).noquote() << "Camera" << (listed == cameras.end() ? id : listed->name) << "opened as" << openedMode << "when asked for" << (camera.sizeAskedFor ? camera.sizeAskedFor : "nothing");
    opened(openedMode);

    const PacketPointer packet(av_packet_alloc());
    const PacketPointer newer(av_packet_alloc());
    const FramePointer decoded(av_frame_alloc());
    const AVCodecDescriptor *codec = avcodec_descriptor_get(camera.decoder->codec_id);
    const bool everyPictureStandsAlone = codec && (codec->props & AV_CODEC_PROP_INTRA_ONLY);
    CaptureClock clock;
    QElapsedTimer sincePicture;
    sincePicture.start();

    while (!stopping) {
        const int got = av_read_frame(camera.input.get(), packet.get());
        if (got == AVERROR(EAGAIN)) {
            if (sincePicture.elapsed() >= NO_PICTURE_TIMEOUT_MS) {
                end(cameraText("The camera is not sending pictures. Another program may be using it."));
                return;
            }
            std::this_thread::sleep_for(POLL_INTERVAL);
            continue;
        }
        if (got < 0) {
            end(cameraText("The camera was disconnected."));
            return;
        }
        sincePicture.start();
        const Clock::time_point readAt = Clock::now();

        while (everyPictureStandsAlone && av_read_frame(camera.input.get(), newer.get()) >= 0) {
            av_packet_unref(packet.get());
            av_packet_move_ref(packet.get(), newer.get());
        }

#ifdef Q_OS_LINUX
        if (!copyOutOfTheDriver(packet.get())) {
            av_packet_unref(packet.get());
            continue;
        }
#endif
        const int sent = avcodec_send_packet(camera.decoder.get(), packet.get());
        av_packet_unref(packet.get());
        while (sent >= 0 && avcodec_receive_frame(camera.decoder.get(), decoded.get()) >= 0) {
            std::optional<std::chrono::microseconds> stamped;
            if (decoded->pts != AV_NOPTS_VALUE)
                stamped = std::chrono::microseconds(av_rescale_q(decoded->pts, stream->time_base, AVRational{ 1, 1'000'000 }));
            publish(decoded.get(), clock.at(stamped, readAt));
        }
    }
}

#else

bool cameraCaptureAvailable()
{
    return false;
}

QList<CameraDevice> cameraDevices()
{
    return {};
}

#endif

} // namespace Video
} // namespace Core
} // namespace Acheron
