#include "Core/Media/VideoEncoder.hpp"

#include "Core/Logging.hpp"
#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Media/H264Sps.hpp"
#include "Core/Media/HardwareDevices.hpp"

#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace Acheron {
namespace Core {
namespace Media {

using Discord::Voice::EncodedVideoFrame;
using Discord::Voice::VideoCodec;

enum class EncoderFamily {
    Amf,
    Nvenc,
    Qsv,
    MediaFoundation,
    Vaapi,
    Vpx,
    X264,
    OpenH264,
};

struct VideoEncoderCandidate
{
    // ffmpeg's name for the encoder, except where one ffmpeg encoder is two candidates.
    const char *label;
    const char *ffmpegName;
    VideoCodec codec;
    EncoderFamily family;
    bool hardware;
    // Which pictures it takes as frames on the graphics card: NV12 ones, and the screen's own BGRA.
    bool takesCardFrames;
    bool takesRgbCardFrames;
    // Whether a new bit rate written into its context is picked up while it runs.
    bool followsBitrate;
};

namespace {

// In the order they are tried, each codec's by themselves. AV1 and H265 are encoded by the
// graphics card or not at all, as in the official client: ffmpeg's software encoders for them
// are left alone. Media Foundation is the graphics card's encoder or Microsoft's software one,
// by an option. The software one runs pictures behind (sixteen, where it was measured) whatever
// it is told through ffmpeg, so the trial turns it down. libopenh264 and x264 are left out on
// Windows on purpose.
#if defined(Q_OS_WIN)
constexpr std::array CANDIDATES = {
    VideoEncoderCandidate{ "av1_amf", "av1_amf", VideoCodec::AV1, EncoderFamily::Amf, true, true, true, false },
    VideoEncoderCandidate{ "av1_nvenc", "av1_nvenc", VideoCodec::AV1, EncoderFamily::Nvenc, true, true, true, true },
    VideoEncoderCandidate{ "av1_qsv", "av1_qsv", VideoCodec::AV1, EncoderFamily::Qsv, true, false, false, true },
    VideoEncoderCandidate{ "av1_mf hardware", "av1_mf", VideoCodec::AV1, EncoderFamily::MediaFoundation, true, true, false, false },
    VideoEncoderCandidate{ "hevc_amf", "hevc_amf", VideoCodec::H265, EncoderFamily::Amf, true, true, true, false },
    VideoEncoderCandidate{ "hevc_nvenc", "hevc_nvenc", VideoCodec::H265, EncoderFamily::Nvenc, true, true, true, true },
    VideoEncoderCandidate{ "hevc_qsv", "hevc_qsv", VideoCodec::H265, EncoderFamily::Qsv, true, false, false, true },
    VideoEncoderCandidate{ "hevc_mf hardware", "hevc_mf", VideoCodec::H265, EncoderFamily::MediaFoundation, true, true, false, false },
    VideoEncoderCandidate{ "h264_amf", "h264_amf", VideoCodec::H264, EncoderFamily::Amf, true, true, true, false },
    VideoEncoderCandidate{ "h264_nvenc", "h264_nvenc", VideoCodec::H264, EncoderFamily::Nvenc, true, true, true, true },
    VideoEncoderCandidate{ "h264_qsv", "h264_qsv", VideoCodec::H264, EncoderFamily::Qsv, true, false, false, true },
    VideoEncoderCandidate{ "h264_mf hardware", "h264_mf", VideoCodec::H264, EncoderFamily::MediaFoundation, true, true, false, false },
    VideoEncoderCandidate{ "h264_mf", "h264_mf", VideoCodec::H264, EncoderFamily::MediaFoundation, false, false, false, false },
    VideoEncoderCandidate{ "libvpx", "libvpx", VideoCodec::VP8, EncoderFamily::Vpx, false, false, false, false },
};
#else
constexpr std::array CANDIDATES = {
    VideoEncoderCandidate{ "av1_vaapi", "av1_vaapi", VideoCodec::AV1, EncoderFamily::Vaapi, true, true, false, false },
    VideoEncoderCandidate{ "av1_nvenc", "av1_nvenc", VideoCodec::AV1, EncoderFamily::Nvenc, true, true, false, true },
    VideoEncoderCandidate{ "hevc_vaapi", "hevc_vaapi", VideoCodec::H265, EncoderFamily::Vaapi, true, true, false, false },
    VideoEncoderCandidate{ "hevc_nvenc", "hevc_nvenc", VideoCodec::H265, EncoderFamily::Nvenc, true, true, false, true },
    VideoEncoderCandidate{ "h264_vaapi", "h264_vaapi", VideoCodec::H264, EncoderFamily::Vaapi, true, true, false, false },
    VideoEncoderCandidate{ "h264_nvenc", "h264_nvenc", VideoCodec::H264, EncoderFamily::Nvenc, true, true, false, true },
    VideoEncoderCandidate{ "libx264", "libx264", VideoCodec::H264, EncoderFamily::X264, false, false, false, true },
    VideoEncoderCandidate{ "libopenh264", "libopenh264", VideoCodec::H264, EncoderFamily::OpenH264, false, false, false, false },
    VideoEncoderCandidate{ "libvpx", "libvpx", VideoCodec::VP8, EncoderFamily::Vpx, false, false, false, false },
};
#endif

// The order the voice server prefers them in when every viewer decodes them.
constexpr std::array CODECS_BEST_FIRST = { VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 };

constexpr int RTP_CLOCK = 90000;
constexpr int X264_BUFFER_FRAMES = 5;

// ffmpeg's numbers for the profiles, which it has renamed between versions.
constexpr int H264_PROFILE_MAIN = 77;
constexpr int H264_PROFILE_HIGH = 100;
constexpr std::array H264_PROFILES = { H264_PROFILE_HIGH, H264_PROFILE_MAIN };
constexpr int H265_PROFILE_MAIN = 1;
constexpr int AV1_PROFILE_MAIN = 0;

// The official client stops its encoders at 56 of VP8's 63 quantizer steps. These are the same
// share of the 51 of H264 and H265, and the place AV1's 255 have for that step.
constexpr int VP8_MIN_QUANTIZER = 2;
constexpr int VP8_MAX_QUANTIZER = 56;
constexpr int H26X_MAX_QP = 45;
constexpr int AV1_MAX_QUANTIZER_INDEX = 224;

constexpr std::array<uint8_t, 4> START_CODE = { 0, 0, 0, 1 };

constexpr int OBU_SEQUENCE_HEADER = 1;
constexpr int OBU_TEMPORAL_DELIMITER = 2;
constexpr int OBU_FRAME_HEADER = 3;
constexpr int OBU_FRAME = 6;
constexpr uint8_t OBU_HAS_EXTENSION = 0x04;
constexpr uint8_t OBU_HAS_SIZE = 0x02;
// ISO/IEC 14496-15's wrapping of a sequence header, which some encoders give as extradata,
// opens with a marker bit and a version of one, and has the OBUs after four bytes.
constexpr uint8_t AV1_CONFIGURATION_MARKER = 0x81;
constexpr size_t AV1_CONFIGURATION_SIZE = 4;

const QLatin1String AMF_AV1_ENCODER("av1_amf");
const QLatin1String VAAPI_AV1_ENCODER("av1_vaapi");
// The blocks AMD's hardware codes AV1 in where it is not free to code any size. Rows that stop
// half a block short are made up with two and not eight, so that 1080 lines come out as 1082:
// ffmpeg's amfenc_av1.c has the same exception in the crop it reports ("special processing
// for crop_bottom equal to 8 in hardware").
constexpr QSize AMF_AV1_BLOCK(64, 16);
constexpr int AMF_AV1_ROWS_FOR_HALF_A_BLOCK = 2;
constexpr int TRIAL_WIDTH = 640;
constexpr int TRIAL_HEIGHT = 360;
// A size that blocks of 64 by 16 and blocks of 8 by 2 each make another of, as the trial's own
// is one for the first only. No narrower than 320, the least a Radeon of the 9000 series codes.
constexpr QSize TRIAL_ODD_SIZE(364, 196);
std::atomic<bool> vaapiAv1CodesEverySizeAsItIs{ false };
constexpr int TRIAL_PICTURES = 30;
constexpr int TRIAL_BITRATE = 1'000'000;
// The pictures the rate is measured over: the ones after rate control has got over the key frame.
constexpr int TRIAL_SETTLING = 10;
// Rate control drops a picture now and then, even one of these.
constexpr int TRIAL_DROPS_ALLOWED = 3;
constexpr double TRIAL_MOST_OF_THE_RATE = 2.0;
constexpr double TRIAL_LEAST_OF_THE_RATE = 0.25;
// Many times what opening the slowest encoder and encoding the trial's pictures takes.
constexpr std::chrono::seconds TRIAL_TIME_ALLOWED(10);
// How long an encoder that had been taking pictures on the card gets them in memory after it
// turns one down.
constexpr std::chrono::seconds CARD_FRAMES_RETRY_AFTER(30);

struct Nal
{
    size_t begin;
    size_t end;
};

// The NAL units of an Annex B stream, without their start codes.
std::vector<Nal> splitNals(std::span<const uint8_t> data)
{
    std::vector<Nal> nals;
    for (size_t i = 0; i + 3 <= data.size();) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) {
            i++;
            continue;
        }
        if (!nals.empty())
            nals.back().end = data[i - 1] == 0 ? i - 1 : i;
        nals.push_back({ i + 3, data.size() });
        i += 3;
    }
    return nals;
}

bool startsWithStartCode(std::span<const uint8_t> data, const std::vector<Nal> &nals)
{
    return !nals.empty() && (nals.front().begin == 3 || (nals.front().begin == 4 && data[0] == 0));
}

// The parameter sets come first, in the order a key frame has them in.
enum class NalKind {
    Vps,
    Sps,
    Pps,
    AccessUnitDelimiter,
    // A picture that needs none before it: an IDR in H264, any IRAP picture in H265.
    KeyPicture,
    Other,
};

bool isParameterSet(NalKind kind)
{
    return kind <= NalKind::Pps;
}

NalKind nalKind(VideoCodec codec, std::span<const uint8_t> data, const Nal &nal)
{
    if (nal.begin >= nal.end)
        return NalKind::Other;
    if (codec == VideoCodec::H265) {
        const int type = (data[nal.begin] >> 1) & 0x3F;
        switch (type) {
        case 32:
            return NalKind::Vps;
        case 33:
            return NalKind::Sps;
        case 34:
            return NalKind::Pps;
        case 35:
            return NalKind::AccessUnitDelimiter;
        default:
            return type >= 16 && type <= 23 ? NalKind::KeyPicture : NalKind::Other;
        }
    }
    switch (data[nal.begin] & 0x1F) {
    case 5:
        return NalKind::KeyPicture;
    case 7:
        return NalKind::Sps;
    case 8:
        return NalKind::Pps;
    case 9:
        return NalKind::AccessUnitDelimiter;
    default:
        return NalKind::Other;
    }
}

// The parameter sets a key frame opens with: H264 has no VPS.
std::span<const NalKind> parameterSetKinds(VideoCodec codec)
{
    static constexpr std::array KINDS = { NalKind::Vps, NalKind::Sps, NalKind::Pps };
    return std::span<const NalKind>(KINDS).subspan(codec == VideoCodec::H265 ? 0 : 1);
}

void appendNal(std::vector<uint8_t> &to, std::span<const uint8_t> nal)
{
    to.insert(to.end(), START_CODE.begin(), START_CODE.end());
    to.insert(to.end(), nal.begin(), nal.end());
}

struct Obu
{
    int type;
    // The header is one byte, or two with its extension, and the size comes between it and
    // the payload when the OBU says its size.
    size_t header;
    size_t headerSize;
    size_t payload;
    size_t end;
    bool saysItsSize;
};

// The OBUs of a temporal unit, or none when it is not one. Only the last may leave its size
// out, and then runs to the end.
std::vector<Obu> splitObus(std::span<const uint8_t> data)
{
    std::vector<Obu> obus;
    for (size_t at = 0; at < data.size();) {
        const uint8_t header = data[at];
        const size_t headerSize = (header & OBU_HAS_EXTENSION) ? 2 : 1;
        size_t payload = at + headerSize;
        if ((header & 0x80) || payload > data.size())
            return {};

        uint64_t size = data.size() - payload;
        const bool saysItsSize = header & OBU_HAS_SIZE;
        if (saysItsSize) {
            size = 0;
            bool more = true;
            for (int shift = 0; more && shift < 56; shift += 7) {
                if (payload == data.size())
                    return {};
                size |= uint64_t(data[payload] & 0x7F) << shift;
                more = data[payload++] & 0x80;
            }
            if (more || size > data.size() - payload)
                return {};
        }
        obus.push_back({ (header >> 3) & 0x0F, at, headerSize, payload, payload + static_cast<size_t>(size), saysItsSize });
        at = obus.back().end;
    }
    return obus;
}

void appendObu(std::vector<uint8_t> &to, std::span<const uint8_t> header, std::span<const uint8_t> payload)
{
    to.push_back(header[0] | OBU_HAS_SIZE);
    to.insert(to.end(), header.begin() + 1, header.end());
    size_t size = payload.size();
    do {
        to.push_back(static_cast<uint8_t>((size & 0x7F) | (size > 0x7F ? 0x80 : 0)));
        size >>= 7;
    } while (size > 0);
    to.insert(to.end(), payload.begin(), payload.end());
}

// Whether a frame header, which is what a frame OBU opens with too, is that of a key frame
// that is shown. A sequence header whose fifth bit is set makes every frame one.
bool isAv1KeyFrame(std::span<const uint8_t> frameHeader, std::span<const uint8_t> sequenceHeader)
{
    if (!sequenceHeader.empty() && (sequenceHeader[0] & 0x08))
        return true;
    if (frameHeader.empty())
        return false;
    const bool showsExistingFrame = frameHeader[0] & 0x80;
    const int frameType = (frameHeader[0] >> 5) & 0x03;
    const bool shown = frameHeader[0] & 0x10;
    return !showsExistingFrame && frameType == 0 && shown;
}

// The thread counts libwebrtc gives libvpx, short of the eight it uses on the largest machines.
int softwareThreads(int width, int height)
{
    const int cores = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const int pixels = width * height;
    if (pixels > 1280 * 960 && cores >= 6)
        return 3;
    if (pixels > 640 * 480 && cores >= 3)
        return 2;
    return 1;
}

QSize inWholeAmfAv1Blocks(const QSize &size)
{
    const int columns = (AMF_AV1_BLOCK.width() - size.width() % AMF_AV1_BLOCK.width()) % AMF_AV1_BLOCK.width();
    int rows = (AMF_AV1_BLOCK.height() - size.height() % AMF_AV1_BLOCK.height()) % AMF_AV1_BLOCK.height();
    if (rows == AMF_AV1_BLOCK.height() / 2)
        rows = AMF_AV1_ROWS_FOR_HALF_A_BLOCK;
    return size + QSize(columns, rows);
}

// A drifting gradient, a patch of texture that slides over it, and a band of noise that is new
// in every picture: more than the trial's bit rate pays for, so that rate control has to hold
// the encoder to it.
void paintTrialPicture(AVFrame *picture, int index)
{
    uint32_t noise = 12345u + static_cast<uint32_t>(index) * 2654435761u;
    for (int y = 0; y < TRIAL_HEIGHT; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        for (int x = 0; x < TRIAL_WIDTH; x++) {
            row[x] = static_cast<uint8_t>((x + y) / 4 + index * 3);
            if (x >= TRIAL_WIDTH / 4 && x < TRIAL_WIDTH * 3 / 4 && y >= TRIAL_HEIGHT / 3 && y < TRIAL_HEIGHT * 2 / 3)
                row[x] = static_cast<uint8_t>(((x + 3 * index) * 7) ^ ((y + 2 * index) * 5));
            if (x < TRIAL_WIDTH / 4 && y < TRIAL_HEIGHT / 6) {
                noise = noise * 1664525u + 1013904223u;
                row[x] = static_cast<uint8_t>(96 + (noise >> 26));
            }
        }
    }
    for (int plane = 1; plane < AV_NUM_DATA_POINTERS && picture->data[plane]; plane++)
        memset(picture->data[plane], 128, static_cast<size_t>(picture->linesize[plane]) * (TRIAL_HEIGHT / 2));
}

// What the encoder codes one picture of a size as, by its own account. Empty when it has none.
QSize codedInTrial(VideoEncoder &encoder, const QSize &size, int64_t timestamp)
{
    AVFrame *picture = av_frame_alloc();
    picture->format = encoder.softwareFormat();
    picture->width = size.width();
    picture->height = size.height();
    std::vector<EncodedVideoFrame> frames;
    if (av_frame_get_buffer(picture, 0) >= 0)
        encoder.encode(picture, timestamp, frames);
    av_frame_free(&picture);
    return frames.empty() ? QSize() : av1CodedSize(frames.front().data);
}

bool encodesTrialPictures(const VideoEncoderCandidate &candidate)
{
    if (!avcodec_find_encoder_by_name(candidate.ffmpegName))
        return false;

    VideoEncoder::Settings settings;
    settings.codec = candidate.codec;
    settings.bitrate = TRIAL_BITRATE;
    settings.encoderName = QString::fromLatin1(candidate.label);
    VideoEncoder encoder(settings);

    const FramePointer picture(av_frame_alloc());
    if (!picture)
        return false;
    picture->format = encoder.softwareFormat();
    picture->width = TRIAL_WIDTH;
    picture->height = TRIAL_HEIGHT;
    bool worked = av_frame_get_buffer(picture.get(), 0) >= 0;

    std::vector<EncodedVideoFrame> frames;
    size_t settledBytes = 0;
    for (int index = 0; worked && index < TRIAL_PICTURES; index++) {
        if (av_frame_make_writable(picture.get()) < 0)
            return false;
        paintTrialPicture(picture.get(), index);

        const int64_t timestamp = int64_t(index) * RTP_CLOCK / settings.framerate;
        const size_t before = frames.size();
        worked = encoder.encode(picture.get(), timestamp, frames) == VideoEncoder::Outcome::Encoded;
        // A frame that comes out a picture late would stay late for as long as the stream runs.
        for (size_t i = before; i < frames.size(); i++) {
            worked = worked && frames[i].rtpTimestamp == static_cast<uint32_t>(timestamp);
            settledBytes += index >= TRIAL_SETTLING ? frames[i].data.size() : 0;
        }
    }
    if (!worked || frames.empty() || !frames.front().keyframe || frames.size() + TRIAL_DROPS_ALLOWED < TRIAL_PICTURES)
        return false;

    // An encoder that takes no notice of the rate it is given would send several times what
    // the connection was measured to carry, or a fraction of what it could.
    const double rate = static_cast<double>(settledBytes) * 8.0 * settings.framerate / (TRIAL_PICTURES - TRIAL_SETTLING);
    if (rate > TRIAL_BITRATE * TRIAL_MOST_OF_THE_RATE || rate < TRIAL_BITRATE * TRIAL_LEAST_OF_THE_RATE) {
        qCWarning(LogVideo) << "Encoder" << candidate.label << "was asked for" << TRIAL_BITRATE / 1000 << "kbps and made" << qRound(rate / 1000);
        return false;
    }

    if (QLatin1String(candidate.label) == VAAPI_AV1_ENCODER) {
        const QList<Av1SizeProbe> probes = {
            { QSize(TRIAL_WIDTH, TRIAL_HEIGHT), av1CodedSize(frames.front().data) },
            { TRIAL_ODD_SIZE, codedInTrial(encoder, TRIAL_ODD_SIZE, int64_t(TRIAL_PICTURES) * RTP_CLOCK / settings.framerate) },
        };
        vaapiAv1CodesEverySizeAsItIs = codesEverySizeAsItIs(probes);
        for (const Av1SizeProbe &probe : probes)
            qCInfo(LogVideo) << "Encoder" << candidate.label << "codes" << probe.given << "as" << probe.coded;
    }
    return true;
}

// An encoder may also take its first picture and never answer, inside ffmpeg, where nothing
// can call it back: Media Foundation's AV1 encoder for a Radeon does. So the trial has a thread
// of its own, which is left to itself when it has not finished in time.
bool encodesTrialPicturesInTime(const VideoEncoderCandidate &candidate)
{
    const auto verdict = std::make_shared<std::promise<bool>>();
    std::future<bool> finished = verdict->get_future();
    std::thread([verdict, &candidate] { verdict->set_value(encodesTrialPictures(candidate)); }).detach();
    if (finished.wait_for(TRIAL_TIME_ALLOWED) == std::future_status::ready)
        return finished.get();
    qCWarning(LogVideo) << "Encoder" << candidate.label << "has not finished its trial after" << TRIAL_TIME_ALLOWED.count() << "seconds";
    return false;
}

// Whether the encoder opens and encodes on this machine: tried once, and remembered. A trial
// holds up only those asking about the same encoder.
bool candidateWorks(const VideoEncoderCandidate &candidate)
{
    static std::array<std::once_flag, CANDIDATES.size()> tried;
    static std::array<bool, CANDIDATES.size()> works = {};

    const size_t index = static_cast<size_t>(&candidate - CANDIDATES.data());
    std::call_once(tried[index], [&candidate, index] {
        works[index] = encodesTrialPicturesInTime(candidate);
        qCInfo(LogVideo) << "Encoder" << candidate.label << (works[index] ? "works" : "does not work on this machine");
    });
    return works[index];
}

} // namespace

QList<Discord::Voice::VideoCodec> encodableVideoCodecs()
{
    static const QList<VideoCodec> codecs = [] {
        QList<VideoCodec> found;
        for (VideoCodec codec : CODECS_BEST_FIRST) {
            if (std::any_of(CANDIDATES.begin(), CANDIDATES.end(), [codec](const VideoEncoderCandidate &each) { return each.codec == codec && candidateWorks(each); }))
                found.append(codec);
        }
        return found;
    }();
    return codecs;
}

QList<QString> videoEncoderNames(Discord::Voice::VideoCodec codec)
{
    QList<QString> names;
    for (const VideoEncoderCandidate &each : CANDIDATES) {
        if (each.codec == codec && avcodec_find_encoder_by_name(each.ffmpegName))
            names.append(QString::fromLatin1(each.label));
    }
    return names;
}

bool videoEncoderWorks(const QString &name)
{
    return std::any_of(CANDIDATES.begin(), CANDIDATES.end(), [&name](const VideoEncoderCandidate &each) { return name == QLatin1String(each.label) && candidateWorks(each); });
}

QString videoEncoderFor(Discord::Voice::VideoCodec codec)
{
    const auto chosen = std::find_if(CANDIDATES.begin(), CANDIDATES.end(), [codec](const VideoEncoderCandidate &each) { return each.codec == codec && candidateWorks(each); });
    return chosen == CANDIDATES.end() ? QString() : QString::fromLatin1(chosen->label);
}

QSize av1CodedSize(const std::vector<uint8_t> &keyFrame)
{
    const std::vector<Obu> obus = splitObus(keyFrame);
    const auto header = std::find_if(obus.begin(), obus.end(), [](const Obu &obu) { return obu.type == OBU_SEQUENCE_HEADER; });
    if (header == obus.end())
        return {};

    size_t at = header->payload * 8;
    bool ended = false;
    const auto bits = [&](uint32_t count) {
        uint32_t value = 0;
        for (; count > 0; count--, at++) {
            ended = ended || at >= header->end * 8;
            value = value << 1 | (ended ? 0u : (keyFrame[at / 8] >> (7 - at % 8)) & 1u);
        }
        return value;
    };

    // sequence_header_obu() of the AV1 specification, section 5.5.1, as far as the size.
    bits(4);
    const bool reducedStillPicture = bits(1);
    const bool timingInfo = !reducedStillPicture && bits(1);
    if (timingInfo)
        return {};
    const bool displayDelays = !reducedStillPicture && bits(1);
    for (uint32_t operatingPoints = reducedStillPicture ? 0 : bits(5) + 1; operatingPoints > 0; operatingPoints--) {
        bits(12);
        const bool hasTier = bits(5) > 7;
        bits(hasTier ? 1 : 0);
        if (displayDelays && bits(1))
            bits(4);
    }
    if (reducedStillPicture)
        bits(5);
    const uint32_t widthBits = bits(4) + 1;
    const uint32_t heightBits = bits(4) + 1;
    const int width = static_cast<int>(bits(widthBits)) + 1;
    const int height = static_cast<int>(bits(heightBits)) + 1;
    return ended ? QSize() : QSize(width, height);
}

bool codesEverySizeAsItIs(const QList<Av1SizeProbe> &probes)
{
    return !probes.isEmpty() && std::all_of(probes.begin(), probes.end(), [](const Av1SizeProbe &probe) { return probe.coded == probe.given; });
}

QSize codedSize(const QString &encoderName, const QSize &size)
{
    if (encoderName == VAAPI_AV1_ENCODER)
        return videoEncoderWorks(encoderName) && vaapiAv1CodesEverySizeAsItIs ? size : inWholeAmfAv1Blocks(size);
    return encoderName == AMF_AV1_ENCODER ? inWholeAmfAv1Blocks(size) : size;
}

bool paddingIsAcceptable(const QSize &size, const QSize &coded)
{
    return coded.width() == size.width() && coded.height() >= size.height() && coded.height() - size.height() <= MOST_ROWS_ADDED;
}

namespace {

// For an encoder that has to start over to change its rate.
constexpr double RISE_WORTH_A_KEY_FRAME = 1.15;
constexpr double ROOM_UNDER_A_LOWERED_RATE = 0.9;

} // namespace

VideoEncoder::VideoEncoder(const Settings &settings)
    : settings(settings), uploaded(av_frame_alloc()), packet(av_packet_alloc()), asEncoded(av_packet_alloc()), bitrateChosenFor(settings.bitrate)
{
    for (const VideoEncoderCandidate &each : CANDIDATES) {
        if (each.codec != settings.codec)
            continue;
        if (settings.encoderName.isEmpty() ? candidateWorks(each) : settings.encoderName == QLatin1String(each.label)) {
            candidate = &each;
            break;
        }
    }
}

VideoEncoder::~VideoEncoder()
{
    close();
    av_frame_free(&uploaded);
    av_packet_free(&packet);
    av_packet_free(&asEncoded);
}

QSize VideoEncoder::codedSize(const QSize &size) const
{
    return candidate ? Media::codedSize(QString::fromLatin1(candidate->label), size) : size;
}

bool VideoEncoder::takesCardFrames() const
{
    const bool refused = cardFramesRefusedUntil && std::chrono::steady_clock::now() < *cardFramesRefusedUntil;
    return candidate && candidate->takesCardFrames && !refused;
}

bool VideoEncoder::takesRgbCardFrames() const
{
    return takesCardFrames() && candidate->takesRgbCardFrames;
}

int VideoEncoder::softwareFormat() const
{
    return candidate && candidate->family == EncoderFamily::Vpx ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12;
}

void VideoEncoder::requestKeyframe()
{
    keyframeWanted = true;
}

void VideoEncoder::setBitrate(int bitrate)
{
    if (!context || candidate->followsBitrate) {
        if (bitrate == settings.bitrate)
            return;
        settings.bitrate = bitrate;
        bitrateChosenFor = bitrate;
        if (context) {
            context->bit_rate = bitrate;
            context->rc_max_rate = bitrate;
            context->rc_buffer_size = bufferBits();
        }
        return;
    }

    const bool runsOver = settings.bitrate > bitrate;
    const bool risenEnough = bitrate >= bitrateChosenFor * RISE_WORTH_A_KEY_FRAME;
    if (!runsOver && !risenEnough)
        return;
    settings.bitrate = runsOver ? static_cast<int>(bitrate * ROOM_UNDER_A_LOWERED_RATE) : bitrate;
    bitrateChosenFor = bitrate;
    reopenWanted = true;
}

VideoEncoder::Outcome VideoEncoder::encode(AVFrame *frame, int64_t timestamp, std::vector<EncodedVideoFrame> &out)
{
    const bool onCard = frame->hw_frames_ctx != nullptr;
    if (!onCard && candidate && frame->format != softwareFormat())
        return Outcome::Failed;

    const bool rgbOnCard = onCard && reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data)->sw_format != AV_PIX_FMT_NV12;
    bool freshStartTried = false;
    while (candidate) {
        if (onCard && !(rgbOnCard ? takesRgbCardFrames() : takesCardFrames()))
            return Outcome::CardFramesRefused;
        if (encodeWithOpenEncoder(frame, timestamp, out))
            return Outcome::Encoded;

        // One that had been working gets a fresh start on the same picture. One that fails
        // again straight away is at fault, unless the picture came on the card: that it turns
        // those down says nothing about what it does with pictures from memory. Nor is it at
        // fault for turning down a size it has not had before, such as a window's new one.
        const bool hadBeenWorking = framesByThisEncoder >= TRIAL_FRAMES;
        close();
        if (hadBeenWorking && !std::exchange(freshStartTried, true))
            continue;
        if (onCard) {
            const auto now = std::chrono::steady_clock::now();
            cardFramesRefusedUntil = cardFramesByThisEncoder > 0 ? now + CARD_FRAMES_RETRY_AFTER : std::chrono::steady_clock::time_point::max();
            return Outcome::CardFramesRefused;
        }
        if (hadBeenWorking && QSize(frame->width, frame->height) != lastEncodedSize)
            return Outcome::SizeRefused;
        moveToNextEncoder();
    }
    return Outcome::Failed;
}

void VideoEncoder::moveToNextEncoder()
{
    const VideoEncoderCandidate *failed = candidate;
    candidate = nullptr;
    if (settings.encoderName.isEmpty()) {
        for (const VideoEncoderCandidate *next = failed + 1; next != CANDIDATES.data() + CANDIDATES.size() && !candidate; next++) {
            if (next->codec == failed->codec && candidateWorks(*next))
                candidate = next;
        }
    }
    cardFramesRefusedUntil.reset();
    cardFramesByThisEncoder = 0;
    framesByThisEncoder = 0;
    qCWarning(LogVideo) << "Encoder" << failed->label << "failed;" << (candidate ? "using" : "no other encoder for") << (candidate ? QString::fromLatin1(candidate->label) : Discord::Voice::videoCodecName(failed->codec));
}

bool VideoEncoder::encodeWithOpenEncoder(AVFrame *frame, int64_t timestamp, std::vector<EncodedVideoFrame> &out)
{
    const bool onCard = frame->hw_frames_ctx != nullptr;
    // AMF asserts that a picture on the card belongs to the frames context it was opened with.
    const bool sameSource = context && (onCard ? context->hw_frames_ctx && !uploadFrames && context->hw_frames_ctx->data == frame->hw_frames_ctx->data : uploadFrames || context->pix_fmt == frame->format);
    const bool fits = sameSource && !reopenWanted && context->width == frame->width && context->height == frame->height;
    if (!fits && !open(frame))
        return false;

    QElapsedTimer timer;
    timer.start();

    const bool intervalPassed = settings.keyframeIntervalMs > 0 && timestamp - lastKeyframeAt >= int64_t(settings.keyframeIntervalMs) * RTP_CLOCK / 1000;
    const bool keyframe = framesSinceOpen == 0 || keyframeWanted || intervalPassed;

    AVFrame *input = uploadFrames ? uploadedCopy(frame) : frame;
    if (!input)
        return false;
    // AMF makes whatever kind of picture a frame says it is, so an ordinary one says nothing.
    input->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    input->pts = timestamp;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 30, 100)
    input->duration = RTP_CLOCK / std::max(1, settings.framerate);
#endif

    const int sent = avcodec_send_frame(context, input);
    av_frame_unref(uploaded);
    if (sent < 0)
        return false;
    awaited++;

    const size_t before = out.size();
    if (!collect(out))
        return false;
    framesSinceOpen++;
    framesByThisEncoder++;
    cardFramesByThisEncoder += onCard ? 1 : 0;
    lastEncodedSize = QSize(frame->width, frame->height);

    // Asked for until the picture comes out, which is the next one when nothing has yet.
    if (out.size() > before) {
        keyframeWanted = false;
        if (out[before].keyframe)
            lastKeyframeAt = timestamp;
        else if (keyframe) {
            qCWarning(LogVideo) << "Encoder" << candidate->label << "made no key frame when asked for one; starting it over";
            reopenWanted = true;
        }
    }

    const double elapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    counters.averageEncodeMs += (elapsedMs - counters.averageEncodeMs) / std::min<double>(static_cast<double>(framesSinceOpen), 30.0);
    counters.encoded += out.size() - before;
    return true;
}

bool VideoEncoder::collect(std::vector<EncodedVideoFrame> &out)
{
    QElapsedTimer waited;
    waited.start();

    for (;;) {
        const int received = avcodec_receive_packet(context, packet);
        if (received == AVERROR(EAGAIN)) {
            // AMF takes a picture from memory and answers some milliseconds later, saying
            // "again" until then; each asking waits a millisecond inside AMF, so this does not
            // spin. The others have answered by now or have dropped the picture.
            if (candidate->family != EncoderFamily::Amf || awaited == 0)
                return true;
            if (waited.elapsed() >= OUTPUT_WAIT_MS) {
                awaited = 0;
                return true;
            }
            std::this_thread::yield();
            continue;
        }
        if (received < 0)
            return false;
        if (colourDescriber && packet->size > 0)
            describeColours();

        EncodedVideoFrame encoded;
        encoded.data.assign(packet->data, packet->data + packet->size);
        encoded.rtpTimestamp = static_cast<uint32_t>(packet->pts);
        av_packet_unref(packet);
        awaited = std::max(0, awaited - 1);

        if (encoded.data.empty())
            continue;
        if (!finisher.finish(encoded))
            return false;
        out.push_back(std::move(encoded));
    }
}

// Which encoders repeat their parameter sets, and when, differs by encoder and by how it was
// asked, and so does what each takes a key frame to be.
bool EncodedFrameFinisher::finish(EncodedVideoFrame &frame)
{
    if (frame.data.empty())
        return false;
    switch (codec) {
    case VideoCodec::H264:
    case VideoCodec::H265:
        return finishAnnexB(frame);
    case VideoCodec::AV1:
        return finishAv1(frame);
    case VideoCodec::VP8:
        // It says so itself, in the lowest bit of its first byte.
        frame.keyframe = !(frame.data[0] & 1);
        return true;
    case VideoCodec::Unknown:
        break;
    }
    return false;
}

bool EncodedFrameFinisher::finishAnnexB(EncodedVideoFrame &frame)
{
    if (codec == VideoCodec::H264)
        H264::rewriteSpsIn(frame.data);

    const std::span<const uint8_t> data(frame.data);
    const std::vector<Nal> nals = splitNals(data);
    if (!startsWithStartCode(data, nals))
        return false;

    frame.keyframe = false;
    for (const Nal &nal : nals) {
        const NalKind kind = nalKind(codec, data, nal);
        frame.keyframe = frame.keyframe || kind == NalKind::KeyPicture;
        if (isParameterSet(kind))
            parameterSets[static_cast<size_t>(kind)].assign(data.begin() + nal.begin, data.begin() + nal.end);
    }
    if (!frame.keyframe)
        return true;

    const std::span<const NalKind> opening = parameterSetKinds(codec);
    bool opensWithParameterSets = nals.size() > opening.size();
    for (size_t i = 0; opensWithParameterSets && i < opening.size(); i++)
        opensWithParameterSets = nalKind(codec, data, nals[i]) == opening[i];
    if (opensWithParameterSets)
        return true;

    std::vector<uint8_t> ordered;
    ordered.reserve(frame.data.size() + 256);
    for (NalKind kind : opening) {
        const std::vector<uint8_t> &parameterSet = parameterSets[static_cast<size_t>(kind)];
        if (parameterSet.empty())
            return false;
        appendNal(ordered, parameterSet);
    }
    for (const Nal &nal : nals) {
        const NalKind kind = nalKind(codec, data, nal);
        if (!isParameterSet(kind) && kind != NalKind::AccessUnitDelimiter)
            appendNal(ordered, data.subspan(nal.begin, nal.end - nal.begin));
    }
    frame.data = std::move(ordered);
    return true;
}

bool EncodedFrameFinisher::finishAv1(EncodedVideoFrame &frame)
{
    const std::span<const uint8_t> data(frame.data);
    const std::vector<Obu> obus = splitObus(data);
    const auto payloadOf = [&data](const Obu &obu) { return data.subspan(obu.payload, obu.end - obu.payload); };

    bool everyOneSaysItsSize = !obus.empty();
    bool hasSequenceHeader = false;
    const Obu *picture = nullptr;
    for (const Obu &obu : obus) {
        everyOneSaysItsSize = everyOneSaysItsSize && obu.saysItsSize;
        if (picture)
            continue;
        if (obu.type == OBU_SEQUENCE_HEADER) {
            const std::span<const uint8_t> payload = payloadOf(obu);
            sequenceHeader.assign(payload.begin(), payload.end());
            hasSequenceHeader = true;
        }
        if (obu.type == OBU_FRAME || obu.type == OBU_FRAME_HEADER)
            picture = &obu;
    }
    if (!picture)
        return false;

    frame.keyframe = isAv1KeyFrame(payloadOf(*picture), sequenceHeader);
    const bool lacksSequenceHeader = frame.keyframe && !hasSequenceHeader;
    if (everyOneSaysItsSize && !lacksSequenceHeader)
        return true;
    if (lacksSequenceHeader && sequenceHeader.empty())
        return false;

    std::vector<uint8_t> whole;
    whole.reserve(frame.data.size() + sequenceHeader.size() + 16);
    bool sequenceHeaderDue = lacksSequenceHeader;
    for (const Obu &obu : obus) {
        // A temporal delimiter, where there is one, stays in front of everything.
        if (sequenceHeaderDue && obu.type != OBU_TEMPORAL_DELIMITER) {
            const uint8_t header = OBU_SEQUENCE_HEADER << 3;
            appendObu(whole, std::span<const uint8_t>(&header, 1), sequenceHeader);
            sequenceHeaderDue = false;
        }
        appendObu(whole, data.subspan(obu.header, obu.headerSize), payloadOf(obu));
    }
    frame.data = std::move(whole);
    return true;
}

void EncodedFrameFinisher::rememberParameterSets(const uint8_t *data, size_t size)
{
    if (!data)
        return;

    std::span<const uint8_t> extradata(data, size);
    if (codec == VideoCodec::AV1) {
        if (extradata.size() > AV1_CONFIGURATION_SIZE && extradata[0] == AV1_CONFIGURATION_MARKER)
            extradata = extradata.subspan(AV1_CONFIGURATION_SIZE);
        for (const Obu &obu : splitObus(extradata)) {
            if (obu.type == OBU_SEQUENCE_HEADER)
                sequenceHeader.assign(extradata.begin() + obu.payload, extradata.begin() + obu.end);
        }
        return;
    }

    std::vector<uint8_t> annexB(extradata.begin(), extradata.end());
    if (codec == VideoCodec::H264)
        H264::rewriteSpsIn(annexB);

    const std::vector<Nal> nals = splitNals(annexB);
    if (!startsWithStartCode(annexB, nals))
        return;
    for (const Nal &nal : nals) {
        const NalKind kind = nalKind(codec, annexB, nal);
        if (isParameterSet(kind))
            parameterSets[static_cast<size_t>(kind)].assign(annexB.begin() + nal.begin, annexB.begin() + nal.end);
    }
}

// The official client gives its encoders a buffer of about one frame. libvpx drops pictures
// whenever its buffer runs low, and gets the second libwebrtc gives it. Media Foundation's
// encoders keep to the buffer they are given by leaving pictures out, which they cannot be
// told not to: a Radeon's H265 one then shows nothing new for a second and more after a key
// frame of a screen full of text. Zero leaves them the buffer they choose themselves, with
// which they hold the rate as well and leave nothing out. x264 stays so far under a buffer of
// one frame that a sixth of the bitrate and more goes unused; with five it uses nearly all.
int VideoEncoder::bufferBits() const
{
    if (candidate->family == EncoderFamily::Vpx)
        return settings.bitrate;
    if (candidate->family == EncoderFamily::X264)
        return static_cast<int>(static_cast<qint64>(settings.bitrate) * X264_BUFFER_FRAMES / std::max(1, settings.framerate));
    if (candidate->family == EncoderFamily::MediaFoundation)
        return 0;
    return settings.bitrate / std::max(1, settings.framerate);
}

AVCodecContext *VideoEncoder::configured(const AVFrame *frame, int h264Profile) const
{
    const AVCodec *encoder = avcodec_find_encoder_by_name(candidate->ffmpegName);
    AVCodecContext *made = encoder ? avcodec_alloc_context3(encoder) : nullptr;
    if (!made)
        return nullptr;

    made->width = frame->width;
    made->height = frame->height;
    made->time_base = { 1, RTP_CLOCK };
    made->framerate = { std::max(1, settings.framerate), 1 };
    made->colorspace = frame->colorspace;
    made->color_range = frame->color_range;
    made->color_primaries = frame->color_primaries;
    made->color_trc = frame->color_trc;
    if (uploadFrames) {
        made->pix_fmt = AV_PIX_FMT_VAAPI;
        made->hw_frames_ctx = av_buffer_ref(uploadFrames);
    } else {
        made->pix_fmt = static_cast<AVPixelFormat>(frame->format);
        if (frame->hw_frames_ctx)
            made->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
    }

    // What the official client's encoders are set to: a constant bit rate, no B-frames, and no
    // key frames but the ones asked for.
    made->bit_rate = settings.bitrate;
    made->rc_max_rate = settings.bitrate;
    made->rc_buffer_size = bufferBits();
    made->max_b_frames = 0;
    made->gop_size = INT_MAX;
    made->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // The official client's profiles: High, Main and Main, all of eight bits and 4:2:0.
    void *options = made->priv_data;
    switch (candidate->codec) {
    case VideoCodec::H264:
        made->profile = h264Profile;
        av_opt_set(options, "profile", h264Profile == H264_PROFILE_HIGH ? "high" : "main", 0);
        made->qmax = H26X_MAX_QP;
        break;
    case VideoCodec::H265:
        made->profile = H265_PROFILE_MAIN;
        av_opt_set(options, "profile", "main", 0);
        made->qmax = H26X_MAX_QP;
        break;
    case VideoCodec::AV1:
        made->profile = AV1_PROFILE_MAIN;
        av_opt_set(options, "profile", "main", 0);
        made->qmax = AV1_MAX_QUANTIZER_INDEX;
        break;
    default:
        break;
    }

    // An option an encoder does not have, in this ffmpeg or at all, is left unset.
    switch (candidate->family) {
    case EncoderFamily::Amf:
        av_opt_set(options, "usage", "lowlatency", 0);
        av_opt_set(options, "rc", "cbr", 0);
        av_opt_set_int(options, "async_depth", 1, 0);
        av_opt_set_int(options, "forced_idr", 1, 0);
        // The usage's own preset is the fastest one. This one took no longer over a picture
        // where it was measured, and left H264 and H265 a decibel and more closer to what
        // went in. AV1 came out the same with either.
        av_opt_set(options, "quality", "balanced", 0);
        // The usage also has rate control leave out a picture that the buffer has no room
        // for, as a frame that says nothing changed. H265 does that for a second and more
        // after a key frame of a screen full of text. H264 calls the option by another name.
        av_opt_set_int(options, "skip_frame", 0, 0);
        av_opt_set_int(options, "frame_skipping", 0, 0);
        if (candidate->codec == VideoCodec::AV1) {
            av_opt_set(options, "header_insertion_mode", "frame", 0);
        } else {
            av_opt_set(options, "header_insertion_mode", "idr", 0);
            // AV1 has no B-frames unless asked, and warns on every opening when it is told so
            // on a card that has none at all.
            av_opt_set_int(options, "bf", 0, 0);
            // The encoder's own low latency mode, which took up to a third off the time a
            // picture takes. AV1 has four modes under this name, none of which changed anything.
            av_opt_set_int(options, "latency", 1, 0);
        }
        // Its count of frames between key frames, where none is zero.
        made->gop_size = 0;
        break;
    case EncoderFamily::Nvenc:
        av_opt_set(options, "preset", "p4", 0);
        av_opt_set(options, "tune", "ull", 0);
        av_opt_set(options, "rc", "cbr", 0);
        av_opt_set_int(options, "zerolatency", 1, 0);
        av_opt_set_int(options, "delay", 0, 0);
        av_opt_set_int(options, "forced-idr", 1, 0);
        break;
    case EncoderFamily::Qsv:
        av_opt_set(options, "scenario", settings.content == Content::Camera ? "videoconference" : "displayremoting", 0);
        av_opt_set_int(options, "async_depth", 1, 0);
        av_opt_set_int(options, "forced_idr", 1, 0);
        // It keeps the count in sixteen bits.
        made->gop_size = UINT16_MAX;
        break;
    case EncoderFamily::MediaFoundation:
        av_opt_set_int(options, "hw_encoding", candidate->hardware ? 1 : 0, 0);
        av_opt_set(options, "rate_control", "cbr", 0);
        break;
    case EncoderFamily::Vaapi:
        // Mesa fills the frames of a constant bit rate up to it, those of a still screen too.
        // A variable one whose most is what it aims at holds the rate as well and fills nothing.
        av_opt_set(options, "rc_mode", "VBR", 0);
        av_opt_set_int(options, "async_depth", 1, 0);
        break;
    case EncoderFamily::Vpx:
        // The settings of libwebrtc's VP8 encoder for a screen.
        made->rc_min_rate = settings.bitrate;
        made->rc_initial_buffer_occupancy = settings.bitrate / 2;
        made->qmin = VP8_MIN_QUANTIZER;
        made->qmax = VP8_MAX_QUANTIZER;
        made->thread_count = softwareThreads(frame->width, frame->height);
        av_opt_set(options, "deadline", "realtime", 0);
        av_opt_set_int(options, "cpu-used", -6, 0);
        // Left alone it holds back 25 frames.
        av_opt_set_int(options, "lag-in-frames", 0, 0);
        av_opt_set_int(options, "auto-alt-ref", 0, 0);
        av_opt_set_int(options, "drop-threshold", 30, 0);
        av_opt_set_int(options, "undershoot-pct", 100, 0);
        av_opt_set_int(options, "overshoot-pct", 15, 0);
        av_opt_set_int(options, "static-thresh", 100, 0);
        av_opt_set_int(options, "max-intra-rate", std::max(300, 30 * settings.framerate), 0);
        break;
    case EncoderFamily::X264:
        // Left alone it makes a slice for every core, each of which costs bits the picture
        // then goes without. What holds it to the rate is the buffer it is given.
        made->thread_count = softwareThreads(frame->width, frame->height);
        av_opt_set(options, "preset", "veryfast", 0);
        av_opt_set(options, "tune", "zerolatency", 0);
        av_opt_set_int(options, "forced-idr", 1, 0);
        break;
    case EncoderFamily::OpenH264:
        av_opt_set_int(options, "allow_skip_frames", 1, 0);
        break;
    }
    return made;
}

bool VideoEncoder::open(const AVFrame *frame)
{
    close();
    reopenWanted = false;
    if (candidate->family == EncoderFamily::Vaapi && !frame->hw_frames_ctx && !prepareUpload(frame))
        return false;

    const size_t profiles = candidate->codec == VideoCodec::H264 ? H264_PROFILES.size() : 1;
    for (size_t i = 0; i < profiles && !context; i++) {
        AVCodecContext *made = configured(frame, H264_PROFILES[i]);
        if (made && avcodec_open2(made, nullptr, nullptr) < 0)
            avcodec_free_context(&made);
        context = made;
    }
    if (!context)
        return false;

    if (candidate->family == EncoderFamily::MediaFoundation && frame->colorspace != AVCOL_SPC_UNSPECIFIED)
        openColourDescriber(frame);
    finisher = EncodedFrameFinisher(candidate->codec);
    finisher.rememberParameterSets(context->extradata, static_cast<size_t>(context->extradata_size));
    awaited = 0;

    counters.encoderName = QString::fromLatin1(candidate->label);
    if (frame->hw_frames_ctx)
        counters.encoderName += QStringLiteral(" (%1)").arg(QString::fromUtf8(av_hwdevice_get_type_name(reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data)->device_ctx->type)));
    counters.hardware = candidate->hardware;
    counters.width = frame->width;
    counters.height = frame->height;
    counters.opened++;
    qCInfo(LogVideo) << "Encoding" << Discord::Voice::videoCodecName(candidate->codec) << "with" << counters.encoderName << "at" << frame->width << "x" << frame->height << settings.bitrate / 1000 << "kbps";
    return true;
}

// ffmpeg has no way to tell Media Foundation what colours the pictures are in, and a Radeon's
// encoders then call whatever they are given BT.709. The description is written into such a
// stream afterwards, by the filter ffmpeg has for rewriting it.
void VideoEncoder::openColourDescriber(const AVFrame *frame)
{
    const bool av1 = candidate->codec == VideoCodec::AV1;
    const AVBitStreamFilter *filter = av_bsf_get_by_name(av1 ? "av1_metadata" : candidate->codec == VideoCodec::H265 ? "hevc_metadata"
                                                                                                                     : "h264_metadata");
    if (!filter || av_bsf_alloc(filter, &colourDescriber) < 0)
        return;

    void *options = colourDescriber->priv_data;
    av_opt_set_int(options, "matrix_coefficients", frame->colorspace, 0);
    if (frame->color_primaries != AVCOL_PRI_UNSPECIFIED)
        av_opt_set_int(options, av1 ? "color_primaries" : "colour_primaries", frame->color_primaries, 0);
    if (frame->color_trc != AVCOL_TRC_UNSPECIFIED)
        av_opt_set_int(options, "transfer_characteristics", frame->color_trc, 0);
    if (frame->color_range != AVCOL_RANGE_UNSPECIFIED)
        av_opt_set_int(options, av1 ? "color_range" : "video_full_range_flag", frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0, 0);

    // The filter is given the frames only. Media Foundation's extradata for H265 is an SPS
    // without the VPS it refers to, which the filter will not start on.
    colourDescriber->time_base_in = context->time_base;
    colourDescriber->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
    colourDescriber->par_in->codec_id = context->codec_id;
    if (av_bsf_init(colourDescriber) < 0)
        av_bsf_free(&colourDescriber);
}

// A frame the filter cannot read goes out as the encoder made it, and so do the ones after it.
void VideoEncoder::describeColours()
{
    if (av_packet_ref(asEncoded, packet) < 0)
        return;
    if (av_bsf_send_packet(colourDescriber, packet) < 0 || av_bsf_receive_packet(colourDescriber, packet) < 0) {
        qCWarning(LogVideo) << "The colours of" << candidate->label << "frames could not be written into them";
        av_packet_unref(packet);
        av_packet_move_ref(packet, asEncoded);
        av_bsf_free(&colourDescriber);
    }
    av_packet_unref(asEncoded);
}

// An encoder that only takes pictures from its own kind of device gets them copied there.
bool VideoEncoder::prepareUpload(const AVFrame *frame)
{
    AVBufferRef *device = nullptr;
    if (!hardwareDeviceCanBeAskedFor(AV_HWDEVICE_TYPE_VAAPI) || av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0)
        return false;
    uploadFrames = av_hwframe_ctx_alloc(device);
    av_buffer_unref(&device);
    if (!uploadFrames)
        return false;

    auto *frames = reinterpret_cast<AVHWFramesContext *>(uploadFrames->data);
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = static_cast<AVPixelFormat>(frame->format);
    frames->width = frame->width;
    frames->height = frame->height;
    if (av_hwframe_ctx_init(uploadFrames) < 0) {
        av_buffer_unref(&uploadFrames);
        return false;
    }
    return true;
}

AVFrame *VideoEncoder::uploadedCopy(const AVFrame *frame)
{
    av_frame_unref(uploaded);
    if (av_hwframe_get_buffer(uploadFrames, uploaded, 0) < 0 || av_hwframe_transfer_data(uploaded, frame, 0) < 0)
        return nullptr;
    av_frame_copy_props(uploaded, frame);
    return uploaded;
}

void VideoEncoder::close()
{
    avcodec_free_context(&context);
    av_bsf_free(&colourDescriber);
    av_buffer_unref(&uploadFrames);
    framesSinceOpen = 0;
}

} // namespace Media
} // namespace Core
} // namespace Acheron
