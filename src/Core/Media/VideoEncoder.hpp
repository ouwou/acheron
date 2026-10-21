#pragma once

#include <QList>
#include <QSize>
#include <QString>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "Discord/Voice/VideoFrame.hpp"

struct AVBSFContext;
struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace Acheron {
namespace Core {
namespace Media {

struct VideoEncoderCandidate;

// the first call runs the encoder trials and can take seconds. thread safe
[[nodiscard]] QList<Discord::Voice::VideoCodec> encodableVideoCodecs();

[[nodiscard]] QList<QString> videoEncoderNames(Discord::Voice::VideoCodec codec);
// an encoder that never answers counts as broken
[[nodiscard]] bool videoEncoderWorks(const QString &name);
[[nodiscard]] QString videoEncoderFor(Discord::Voice::VideoCodec codec);

// AMD's AV1 encoder codes some cards' pictures in 64x16 blocks, padding with black. av1_amf is assumed
// to; av1_vaapi is checked in its trial
[[nodiscard]] QSize codedSize(const QString &encoderName, const QSize &size);

[[nodiscard]] QSize av1CodedSize(const std::vector<uint8_t> &keyFrame);

struct Av1SizeProbe
{
    QSize given;
    QSize coded;
};
[[nodiscard]] bool codesEverySizeAsItIs(const QList<Av1SizeProbe> &probes);

inline constexpr int MOST_ROWS_ADDED = 8;
// padding shows black to every viewer: a few rows at the bottom are tolerated, columns arent
[[nodiscard]] bool paddingIsAcceptable(const QSize &size, const QSize &coded);

// H264 SPSs are left in a form the official client doesnt rewrite, since a rewritten one fails DAVE
// decryption (H264Sps.hpp)
class EncodedFrameFinisher
{
public:
    explicit EncodedFrameFinisher(Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown) : codec(codec) {}

    void rememberParameterSets(const uint8_t *extradata, size_t size);
    [[nodiscard]] bool finish(Discord::Voice::EncodedVideoFrame &frame);

private:
    [[nodiscard]] bool finishAnnexB(Discord::Voice::EncodedVideoFrame &frame);
    [[nodiscard]] bool finishAv1(Discord::Voice::EncodedVideoFrame &frame);

    Discord::Voice::VideoCodec codec;
    std::array<std::vector<uint8_t>, 3> parameterSets;
    std::vector<uint8_t> sequenceHeader;
};

// not thread safe
class VideoEncoder
{
public:
    enum class Content {
        Screen,
        Camera,
    };

    struct Settings
    {
        Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown;
        int framerate = 30;
        int bitrateBps = 0;
        Content content = Content::Screen;
        // empty picks the best that works
        QString encoderName;
    };

    enum class Outcome {
        Encoded,
        CardFramesRefused,
        SizeRefused,
        Failed,
    };

    struct Stats
    {
        QString encoderName;
        bool hardware = false;
        quint64 encoded = 0;
        quint64 opened = 0;
        double averageEncodeMs = 0;
    };

    explicit VideoEncoder(const Settings &settings);
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder &) = delete;
    VideoEncoder &operator=(const VideoEncoder &) = delete;

    [[nodiscard]] bool hasEncoder() const { return candidate != nullptr; }
    [[nodiscard]] QSize codedSize(const QSize &size) const;
    [[nodiscard]] bool takesCardFrames() const;
    [[nodiscard]] bool takesRgbCardFrames() const;
    [[nodiscard]] int softwareFormat() const;

    void requestKeyframe();
    // an encoder that cant change rate live is reopened, costing a key frame, so it only follows a large
    // rise and runs a little under a lowered rate
    void setBitrate(int bitrate);

    // `timestamp` is 90 kHz and must increase. overwrites the frame's pts and pict_type
    Outcome encode(AVFrame *frame, int64_t timestamp, std::vector<Discord::Voice::EncodedVideoFrame> &out);

    [[nodiscard]] const Stats &stats() const { return counters; }

    static constexpr int TRIAL_FRAMES = 30;
    static constexpr int OUTPUT_WAIT_MS = 250;

private:
    bool encodeWithOpenEncoder(AVFrame *frame, int64_t timestamp, std::vector<Discord::Voice::EncodedVideoFrame> &out);
    bool open(const AVFrame *frame);
    AVCodecContext *configured(const AVFrame *frame, int h264Profile) const;
    void openColourDescriber(const AVFrame *frame);
    void describeColours();
    bool prepareUpload(const AVFrame *frame);
    AVFrame *uploadedCopy(const AVFrame *frame);
    void close();
    void moveToNextEncoder();
    bool collect(std::vector<Discord::Voice::EncodedVideoFrame> &out);
    [[nodiscard]] int bufferBits() const;

    Settings settings;
    const VideoEncoderCandidate *candidate = nullptr;
    AVCodecContext *context = nullptr;
    AVBufferRef *uploadFrames = nullptr;
    AVFrame *uploaded = nullptr;
    AVPacket *packet = nullptr;
    AVBSFContext *colourDescriber = nullptr;
    AVPacket *asEncoded = nullptr;

    // forever when it never took a card picture
    std::optional<std::chrono::steady_clock::time_point> cardFramesRefusedUntil;
    quint64 cardFramesByThisEncoder = 0;
    bool keyframeWanted = false;
    bool reopenWanted = false;
    int bitrateChosenFor = 0;
    int awaited = 0;
    quint64 framesSinceOpen = 0;
    quint64 framesByThisEncoder = 0;
    QSize lastEncodedSize;
    EncodedFrameFinisher finisher;

    Stats counters;
};

} // namespace Media
} // namespace Core
} // namespace Acheron
