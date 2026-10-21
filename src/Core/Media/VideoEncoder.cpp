#include "Core/Media/VideoEncoder.hpp"

#include "Core/Logging.hpp"
#include "Core/Media/FfmpegPointers.hpp"
#include "Core/Media/H264Sps.hpp"
#include "Core/Media/HardwareDevices.hpp"
#include "Core/Media/RunningAverage.hpp"
#include "Core/Media/BitReader.hpp"
#include "Discord/Voice/Av1Obu.hpp"
#include "Discord/Voice/NalUnits.hpp"

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
    const char *label;
    const char *ffmpegName;
    VideoCodec codec;
    EncoderFamily family;
    bool hardware = false;
    bool takesCardFrames = false;
    bool takesRgbCardFrames = false;
    bool followsBitrate = false;
};

namespace {

// AV1 and H265 are hardware only, as in the official client. Media Foundation's software encoder runs
// about sixteen pictures behind, so its trial fails it. libopenh264 and x264 are left out on Windows
#if defined(Q_OS_WIN)
constexpr std::array CANDIDATES = {
    VideoEncoderCandidate{ .label = "av1_amf", .ffmpegName = "av1_amf", .codec = VideoCodec::AV1, .family = EncoderFamily::Amf, .hardware = true, .takesCardFrames = true, .takesRgbCardFrames = true },
    VideoEncoderCandidate{ .label = "av1_nvenc", .ffmpegName = "av1_nvenc", .codec = VideoCodec::AV1, .family = EncoderFamily::Nvenc, .hardware = true, .takesCardFrames = true, .takesRgbCardFrames = true, .followsBitrate = true },
    VideoEncoderCandidate{ .label = "av1_qsv", .ffmpegName = "av1_qsv", .codec = VideoCodec::AV1, .family = EncoderFamily::Qsv, .hardware = true, .followsBitrate = true },
    VideoEncoderCandidate{ .label = "av1_mf hardware", .ffmpegName = "av1_mf", .codec = VideoCodec::AV1, .family = EncoderFamily::MediaFoundation, .hardware = true, .takesCardFrames = true },
    VideoEncoderCandidate{ .label = "hevc_amf", .ffmpegName = "hevc_amf", .codec = VideoCodec::H265, .family = EncoderFamily::Amf, .hardware = true, .takesCardFrames = true, .takesRgbCardFrames = true },
    VideoEncoderCandidate{ .label = "hevc_nvenc", .ffmpegName = "hevc_nvenc", .codec = VideoCodec::H265, .family = EncoderFamily::Nvenc, .hardware = true, .takesCardFrames = true, .takesRgbCardFrames = true, .followsBitrate = true },
    VideoEncoderCandidate{ .label = "hevc_qsv", .ffmpegName = "hevc_qsv", .codec = VideoCodec::H265, .family = EncoderFamily::Qsv, .hardware = true, .followsBitrate = true },
    VideoEncoderCandidate{ .label = "hevc_mf hardware", .ffmpegName = "hevc_mf", .codec = VideoCodec::H265, .family = EncoderFamily::MediaFoundation, .hardware = true, .takesCardFrames = true },
    VideoEncoderCandidate{ .label = "h264_amf", .ffmpegName = "h264_amf", .codec = VideoCodec::H264, .family = EncoderFamily::Amf, .hardware = true, .takesCardFrames = true, .takesRgbCardFrames = true },
    VideoEncoderCandidate{ .label = "h264_nvenc", .ffmpegName = "h264_nvenc", .codec = VideoCodec::H264, .family = EncoderFamily::Nvenc, .hardware = true, .takesCardFrames = true, .takesRgbCardFrames = true, .followsBitrate = true },
    VideoEncoderCandidate{ .label = "h264_qsv", .ffmpegName = "h264_qsv", .codec = VideoCodec::H264, .family = EncoderFamily::Qsv, .hardware = true, .followsBitrate = true },
    VideoEncoderCandidate{ .label = "h264_mf hardware", .ffmpegName = "h264_mf", .codec = VideoCodec::H264, .family = EncoderFamily::MediaFoundation, .hardware = true, .takesCardFrames = true },
    VideoEncoderCandidate{ .label = "h264_mf", .ffmpegName = "h264_mf", .codec = VideoCodec::H264, .family = EncoderFamily::MediaFoundation },
    VideoEncoderCandidate{ .label = "libvpx", .ffmpegName = "libvpx", .codec = VideoCodec::VP8, .family = EncoderFamily::Vpx },
};
#else
constexpr std::array CANDIDATES = {
    VideoEncoderCandidate{ .label = "av1_vaapi", .ffmpegName = "av1_vaapi", .codec = VideoCodec::AV1, .family = EncoderFamily::Vaapi, .hardware = true, .takesCardFrames = true },
    VideoEncoderCandidate{ .label = "av1_nvenc", .ffmpegName = "av1_nvenc", .codec = VideoCodec::AV1, .family = EncoderFamily::Nvenc, .hardware = true, .takesCardFrames = true, .followsBitrate = true },
    VideoEncoderCandidate{ .label = "hevc_vaapi", .ffmpegName = "hevc_vaapi", .codec = VideoCodec::H265, .family = EncoderFamily::Vaapi, .hardware = true, .takesCardFrames = true },
    VideoEncoderCandidate{ .label = "hevc_nvenc", .ffmpegName = "hevc_nvenc", .codec = VideoCodec::H265, .family = EncoderFamily::Nvenc, .hardware = true, .takesCardFrames = true, .followsBitrate = true },
    VideoEncoderCandidate{ .label = "h264_vaapi", .ffmpegName = "h264_vaapi", .codec = VideoCodec::H264, .family = EncoderFamily::Vaapi, .hardware = true, .takesCardFrames = true },
    VideoEncoderCandidate{ .label = "h264_nvenc", .ffmpegName = "h264_nvenc", .codec = VideoCodec::H264, .family = EncoderFamily::Nvenc, .hardware = true, .takesCardFrames = true, .followsBitrate = true },
    VideoEncoderCandidate{ .label = "libx264", .ffmpegName = "libx264", .codec = VideoCodec::H264, .family = EncoderFamily::X264, .followsBitrate = true },
    VideoEncoderCandidate{ .label = "libopenh264", .ffmpegName = "libopenh264", .codec = VideoCodec::H264, .family = EncoderFamily::OpenH264 },
    VideoEncoderCandidate{ .label = "libvpx", .ffmpegName = "libvpx", .codec = VideoCodec::VP8, .family = EncoderFamily::Vpx },
};
#endif

constexpr int RTP_CLOCK = 90000;
constexpr int X264_BUFFER_FRAMES = 5;

// ffmpeg renamed its profile constants between versions
constexpr int H264_PROFILE_MAIN = 77;
constexpr int H264_PROFILE_HIGH = 100;
constexpr std::array H264_PROFILES = { H264_PROFILE_HIGH, H264_PROFILE_MAIN };
constexpr int H265_PROFILE_MAIN = 1;
constexpr int AV1_PROFILE_MAIN = 0;

// the official client stops VP8 at quantizer 56 of 63; these are the same share for each codec
constexpr int VP8_MIN_QUANTIZER = 2;
constexpr int VP8_MAX_QUANTIZER = 56;
constexpr int H26X_MAX_QP = 45;
constexpr int AV1_MAX_QUANTIZER_INDEX = 224;

using namespace Discord::Voice;
using namespace Discord::Voice::Av1;
// ISO/IEC 14496-15 extradata: 0x81, then the OBUs after four bytes
constexpr uint8_t AV1_CONFIGURATION_MARKER = 0x81;
constexpr size_t AV1_CONFIGURATION_SIZE = 4;

const QLatin1String AMF_AV1_ENCODER("av1_amf");
const QLatin1String VAAPI_AV1_ENCODER("av1_vaapi");
// a last row half a block short is padded by 2, not 8, so 1080 lines code as 1082 (amfenc_av1.c)
constexpr QSize AMF_AV1_BLOCK(64, 16);
constexpr int AMF_AV1_ROWS_FOR_HALF_A_BLOCK = 2;
constexpr int TRIAL_WIDTH = 640;
constexpr int TRIAL_HEIGHT = 360;
// a multiple of 8x2 but not of 64x16, to tell the two apart. 320 is the narrowest a Radeon 9000 codes
constexpr QSize TRIAL_ODD_SIZE(364, 196);
std::atomic<bool> vaapiAv1CodesEverySizeAsItIs{ false };
constexpr int TRIAL_PICTURES = 30;
constexpr int TRIAL_BITRATE = 1'000'000;
constexpr int TRIAL_SETTLING = 10;
constexpr int TRIAL_DROPS_ALLOWED = 3;
constexpr double TRIAL_MOST_OF_THE_RATE = 2.0;
constexpr double TRIAL_LEAST_OF_THE_RATE = 0.25;
constexpr std::chrono::seconds TRIAL_TIME_ALLOWED(10);
constexpr std::chrono::seconds CARD_FRAMES_RETRY_AFTER(30);

bool opensWithStartCode(std::span<const uint8_t> data)
{
    const auto opensWith = [data](std::span<const uint8_t> code) { return data.size() >= code.size() && std::equal(code.begin(), code.end(), data.begin()); };
    return opensWith(START_CODE) || opensWith(std::span(START_CODE).subspan(1));
}

enum class NalKind {
    Vps,
    Sps,
    Pps,
    AccessUnitDelimiter,
    KeyPicture,
    Other,
};

bool isParameterSet(NalKind kind)
{
    return kind <= NalKind::Pps;
}

NalKind nalKind(VideoCodec codec, std::span<const uint8_t> nal)
{
    if (codec == VideoCodec::H265) {
        const uint8_t type = h265Type(nal[0]);
        switch (type) {
        case H265_VPS:
            return NalKind::Vps;
        case H265_SPS:
            return NalKind::Sps;
        case H265_PPS:
            return NalKind::Pps;
        case H265_ACCESS_UNIT_DELIMITER:
            return NalKind::AccessUnitDelimiter;
        default:
            return h265IsIrap(type) ? NalKind::KeyPicture : NalKind::Other;
        }
    }
    switch (h264Type(nal[0])) {
    case H264_IDR:
        return NalKind::KeyPicture;
    case H264_SPS:
        return NalKind::Sps;
    case H264_PPS:
        return NalKind::Pps;
    case H264_ACCESS_UNIT_DELIMITER:
        return NalKind::AccessUnitDelimiter;
    default:
        return NalKind::Other;
    }
}

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

std::vector<ParsedObu> splitObus(std::span<const uint8_t> data)
{
    std::vector<ParsedObu> obus;
    while (!data.empty()) {
        const std::optional<ParsedObu> obu = readObu(data);
        if (!obu || (obu->header[0] & OBU_FORBIDDEN_BIT))
            return {};
        obus.push_back(*obu);
    }
    return obus;
}

void appendObu(std::vector<uint8_t> &to, std::span<const uint8_t> header, std::span<const uint8_t> payload)
{
    to.push_back(header[0] | OBU_HAS_SIZE);
    to.insert(to.end(), header.begin() + 1, header.end());
    appendLeb128(to, payload.size());
    to.insert(to.end(), payload.begin(), payload.end());
}

// reduced_still_picture_header makes every frame a key frame
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

// libwebrtc's thread counts for libvpx
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

QSize codedInTrial(VideoEncoder &encoder, const QSize &size, int64_t timestamp)
{
    const FramePointer picture(av_frame_alloc());
    std::vector<EncodedVideoFrame> frames;
    if (picture && prepareWritable(picture.get(), encoder.softwareFormat(), size) >= 0)
        encoder.encode(picture.get(), timestamp, frames);
    return frames.empty() ? QSize() : av1CodedSize(frames.front().data);
}

bool encodesTrialPictures(const VideoEncoderCandidate &candidate)
{
    if (!avcodec_find_encoder_by_name(candidate.ffmpegName))
        return false;

    VideoEncoder::Settings settings;
    settings.codec = candidate.codec;
    settings.bitrateBps = TRIAL_BITRATE;
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
        for (size_t i = before; i < frames.size(); i++) {
            worked = worked && frames[i].rtpTimestamp == static_cast<uint32_t>(timestamp);
            settledBytes += index >= TRIAL_SETTLING ? frames[i].data.size() : 0;
        }
    }
    if (!worked || frames.empty() || !frames.front().keyframe || frames.size() + TRIAL_DROPS_ALLOWED < TRIAL_PICTURES)
        return false;

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

// Media Foundation's AV1 encoder on a Radeon can take a picture and never return
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
        for (VideoCodec codec : Discord::Voice::VIDEO_CODECS_BEST_FIRST) {
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
    const std::vector<ParsedObu> obus = splitObus(keyFrame);
    const auto header = std::find_if(obus.begin(), obus.end(), [](const ParsedObu &obu) { return obu.type() == OBU_SEQUENCE_HEADER; });
    if (header == obus.end())
        return {};

    BitReader reader(header->body);
    const auto bits = [&reader](uint32_t count) { return reader.bits(static_cast<int>(count)); };

    // AV1 spec 5.5.1
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
    return reader.failed() ? QSize() : QSize(width, height);
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

constexpr double RISE_WORTH_A_KEY_FRAME = 1.15;
constexpr double ROOM_UNDER_A_LOWERED_RATE = 0.9;

} // namespace

VideoEncoder::VideoEncoder(const Settings &settings)
    : settings(settings), uploaded(av_frame_alloc()), packet(av_packet_alloc()), asEncoded(av_packet_alloc()), bitrateChosenFor(settings.bitrateBps)
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
        if (bitrate == settings.bitrateBps)
            return;
        settings.bitrateBps = bitrate;
        bitrateChosenFor = bitrate;
        if (context) {
            context->bit_rate = bitrate;
            context->rc_max_rate = bitrate;
            context->rc_buffer_size = bufferBits();
        }
        return;
    }

    const bool runsOver = settings.bitrateBps > bitrate;
    const bool risenEnough = bitrate >= bitrateChosenFor * RISE_WORTH_A_KEY_FRAME;
    if (!runsOver && !risenEnough)
        return;
    settings.bitrateBps = runsOver ? static_cast<int>(bitrate * ROOM_UNDER_A_LOWERED_RATE) : bitrate;
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
    // AMF asserts that a card picture belongs to the frames context it was opened with
    const bool sameSource = context && (onCard ? context->hw_frames_ctx && !uploadFrames && context->hw_frames_ctx->data == frame->hw_frames_ctx->data : uploadFrames || context->pix_fmt == frame->format);
    const bool fits = sameSource && !reopenWanted && context->width == frame->width && context->height == frame->height;
    if (!fits && !open(frame))
        return false;

    QElapsedTimer timer;
    timer.start();

    const bool keyframe = framesSinceOpen == 0 || keyframeWanted;

    AVFrame *input = uploadFrames ? uploadedCopy(frame) : frame;
    if (!input)
        return false;
    // AMF makes whatever picture type a frame says
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

    if (out.size() > before) {
        keyframeWanted = false;
        if (keyframe && !out[before].keyframe) {
            qCWarning(LogVideo) << "Encoder" << candidate->label << "made no key frame when asked for one; starting it over";
            reopenWanted = true;
        }
    }

    const double elapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    counters.averageEncodeMs = withSample(counters.averageEncodeMs, elapsedMs, framesSinceOpen);
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
            // AMF answers some milliseconds later and says "again" until then; each asking waits a millisecond
            // inside AMF, so this doesnt spin
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
        // VP8: bit 0 of the first byte is clear on a key frame
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
    const std::vector<std::span<const uint8_t>> nals = splitAnnexB(data);
    if (nals.empty() || !opensWithStartCode(data))
        return false;

    frame.keyframe = false;
    for (const std::span<const uint8_t> nal : nals) {
        const NalKind kind = nalKind(codec, nal);
        frame.keyframe = frame.keyframe || kind == NalKind::KeyPicture;
        if (isParameterSet(kind))
            parameterSets[static_cast<size_t>(kind)].assign(nal.begin(), nal.end());
    }
    if (!frame.keyframe)
        return true;

    const std::span<const NalKind> opening = parameterSetKinds(codec);
    bool opensWithParameterSets = nals.size() > opening.size();
    for (size_t i = 0; opensWithParameterSets && i < opening.size(); i++)
        opensWithParameterSets = nalKind(codec, nals[i]) == opening[i];
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
    for (const std::span<const uint8_t> nal : nals) {
        const NalKind kind = nalKind(codec, nal);
        if (!isParameterSet(kind) && kind != NalKind::AccessUnitDelimiter)
            appendNal(ordered, nal);
    }
    frame.data = std::move(ordered);
    return true;
}

bool EncodedFrameFinisher::finishAv1(EncodedVideoFrame &frame)
{
    const std::span<const uint8_t> data(frame.data);
    const std::vector<ParsedObu> obus = splitObus(data);

    bool everyOneSaysItsSize = !obus.empty();
    bool hasSequenceHeader = false;
    const ParsedObu *picture = nullptr;
    for (const ParsedObu &obu : obus) {
        everyOneSaysItsSize = everyOneSaysItsSize && obu.hasSize;
        if (picture)
            continue;
        if (obu.type() == OBU_SEQUENCE_HEADER) {
            sequenceHeader.assign(obu.body.begin(), obu.body.end());
            hasSequenceHeader = true;
        }
        if (obu.type() == OBU_FRAME || obu.type() == OBU_FRAME_HEADER)
            picture = &obu;
    }
    if (!picture)
        return false;

    frame.keyframe = isAv1KeyFrame(picture->body, sequenceHeader);
    const bool lacksSequenceHeader = frame.keyframe && !hasSequenceHeader;
    if (everyOneSaysItsSize && !lacksSequenceHeader)
        return true;
    if (lacksSequenceHeader && sequenceHeader.empty())
        return false;

    std::vector<uint8_t> whole;
    whole.reserve(frame.data.size() + sequenceHeader.size() + 16);
    bool sequenceHeaderDue = lacksSequenceHeader;
    for (const ParsedObu &obu : obus) {
        if (sequenceHeaderDue && obu.type() != OBU_TEMPORAL_DELIMITER) {
            const uint8_t header = OBU_SEQUENCE_HEADER << 3;
            appendObu(whole, std::span<const uint8_t>(&header, 1), sequenceHeader);
            sequenceHeaderDue = false;
        }
        appendObu(whole, obu.header, obu.body);
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
        for (const ParsedObu &obu : splitObus(extradata)) {
            if (obu.type() == OBU_SEQUENCE_HEADER)
                sequenceHeader.assign(obu.body.begin(), obu.body.end());
        }
        return;
    }

    std::vector<uint8_t> annexB(extradata.begin(), extradata.end());
    if (codec == VideoCodec::H264)
        H264::rewriteSpsIn(annexB);

    const std::vector<std::span<const uint8_t>> nals = splitAnnexB(annexB);
    if (nals.empty() || !opensWithStartCode(annexB))
        return;
    for (const std::span<const uint8_t> nal : nals) {
        const NalKind kind = nalKind(codec, nal);
        if (isParameterSet(kind))
            parameterSets[static_cast<size_t>(kind)].assign(nal.begin(), nal.end());
    }
}

// the official client gives its encoders about one frame of buffer, libvpx libwebrtc's one second.
// Media Foundation skips pictures to keep to a buffer, which froze a Radeon's H265 for over a second
// after a key frame of text, so it gets none. x264 left a sixth of the rate unused on one frame
int VideoEncoder::bufferBits() const
{
    if (candidate->family == EncoderFamily::Vpx)
        return settings.bitrateBps;
    if (candidate->family == EncoderFamily::X264)
        return static_cast<int>(static_cast<qint64>(settings.bitrateBps) * X264_BUFFER_FRAMES / std::max(1, settings.framerate));
    if (candidate->family == EncoderFamily::MediaFoundation)
        return 0;
    return settings.bitrateBps / std::max(1, settings.framerate);
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

    // as the official client: constant bit rate, no B-frames, key frames only when asked for
    made->bit_rate = settings.bitrateBps;
    made->rc_max_rate = settings.bitrateBps;
    made->rc_buffer_size = bufferBits();
    made->max_b_frames = 0;
    made->gop_size = INT_MAX;
    made->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // the official client's profiles
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

    switch (candidate->family) {
    case EncoderFamily::Amf:
        av_opt_set(options, "usage", "lowlatency", 0);
        av_opt_set(options, "rc", "cbr", 0);
        av_opt_set_int(options, "async_depth", 1, 0);
        av_opt_set_int(options, "forced_idr", 1, 0);
        av_opt_set(options, "quality", "balanced", 0);
        // otherwise rate control skips pictures, which froze H265 for over a second after a key frame of text
        av_opt_set_int(options, "skip_frame", 0, 0);
        av_opt_set_int(options, "frame_skipping", 0, 0);
        if (candidate->codec == VideoCodec::AV1) {
            av_opt_set(options, "header_insertion_mode", "frame", 0);
        } else {
            av_opt_set(options, "header_insertion_mode", "idr", 0);
            // AV1 warns on every open when told this on a card without B-frames
            av_opt_set_int(options, "bf", 0, 0);
            // took up to a third off encoding time
            av_opt_set_int(options, "latency", 1, 0);
        }
        // no periodic key frames
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
        // 16-bit counter
        made->gop_size = UINT16_MAX;
        break;
    case EncoderFamily::MediaFoundation:
        av_opt_set_int(options, "hw_encoding", candidate->hardware ? 1 : 0, 0);
        av_opt_set(options, "rate_control", "cbr", 0);
        break;
    case EncoderFamily::Vaapi:
        // Mesa pads constant bit rate frames up to the rate, even on a still screen
        av_opt_set(options, "rc_mode", "VBR", 0);
        av_opt_set_int(options, "async_depth", 1, 0);
        break;
    case EncoderFamily::Vpx:
        // libwebrtc's VP8 screen settings
        made->rc_min_rate = settings.bitrateBps;
        made->rc_initial_buffer_occupancy = settings.bitrateBps / 2;
        made->qmin = VP8_MIN_QUANTIZER;
        made->qmax = VP8_MAX_QUANTIZER;
        made->thread_count = softwareThreads(frame->width, frame->height);
        av_opt_set(options, "deadline", "realtime", 0);
        av_opt_set_int(options, "cpu-used", -6, 0);
        // 25 by default
        av_opt_set_int(options, "lag-in-frames", 0, 0);
        av_opt_set_int(options, "auto-alt-ref", 0, 0);
        av_opt_set_int(options, "drop-threshold", 30, 0);
        av_opt_set_int(options, "undershoot-pct", 100, 0);
        av_opt_set_int(options, "overshoot-pct", 15, 0);
        av_opt_set_int(options, "static-thresh", 100, 0);
        av_opt_set_int(options, "max-intra-rate", std::max(300, 30 * settings.framerate), 0);
        break;
    case EncoderFamily::X264:
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
    counters.opened++;
    qCInfo(LogVideo) << "Encoding" << Discord::Voice::videoCodecName(candidate->codec) << "with" << counters.encoderName << "at" << frame->width << "x" << frame->height << settings.bitrateBps / 1000 << "kbps";
    return true;
}

// ffmpeg cant tell Media Foundation the pictures' colours, and a Radeon then marks everything BT.709
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

    // frames only: Media Foundation's H265 extradata is an SPS without its VPS, which the filter rejects
    colourDescriber->time_base_in = context->time_base;
    colourDescriber->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
    colourDescriber->par_in->codec_id = context->codec_id;
    if (av_bsf_init(colourDescriber) < 0)
        av_bsf_free(&colourDescriber);
}

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
