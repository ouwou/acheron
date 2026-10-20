#pragma once

#include <QList>
#include <QSize>
#include <QString>

#include <array>
#include <cstdint>
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

// The codecs this machine can encode, in the order the voice server prefers them: AV1, H265,
// H264, VP8. A codec counts when one of its encoders opens, hands every frame back with the
// picture it was made from and keeps to the bit rate, which is tried once and can take a
// moment. AV1 and H265 count only with the graphics card's encoder, as in the official client.
// Any thread may ask.
[[nodiscard]] QList<Discord::Voice::VideoCodec> encodableVideoCodecs();

// Every encoder of a codec this build could use, best first, whether or not it works on this
// machine: the names VideoEncoder::Settings::encoderName takes.
[[nodiscard]] QList<QString> videoEncoderNames(Discord::Voice::VideoCodec codec);
// Whether the named encoder opens on this machine, hands every frame back with the picture it
// was made from and keeps to the bit rate. Tried once, like the codecs, and given up on after
// some seconds: an encoder may take a picture and never answer.
[[nodiscard]] bool videoEncoderWorks(const QString &name);
// The encoder a codec's pictures go to first: the best of its encoders that works on this
// machine, by the same trial. Empty when none does.
[[nodiscard]] QString videoEncoderFor(Discord::Voice::VideoCodec codec);

// The size the named encoder codes a picture of `size` as. All but two code it as it is. AMD's
// AV1 encoder, on the cards that code it in whole blocks of 64 by 16, fills the last blocks
// with black, and `av1_amf` is taken to be on such a card: the only kind it was measured on.
// `av1_vaapi` is asked in its trial, and taken to be on one unless it codes every size it is
// tried with as it is.
[[nodiscard]] QSize codedSize(const QString &encoderName, const QSize &size);

// The size an AV1 key frame's sequence header says its pictures are coded at. Empty when the
// frame has none that can be read, or one that says when pictures are to be shown.
[[nodiscard]] QSize av1CodedSize(const std::vector<uint8_t> &keyFrame);

// A picture of an encoder's trial: the size it was given at and the size it was coded at.
struct Av1SizeProbe
{
    QSize given;
    QSize coded;
};
// Whether an encoder whose trial went this way codes every size as it is: every picture of it
// did, and there was one.
[[nodiscard]] bool codesEverySizeAsItIs(const QList<Av1SizeProbe> &probes);

inline constexpr int MOST_ROWS_ADDED = 8;
// Whether a picture of `size` may be sent coded as `coded`. What is added is black to every
// viewer, and nothing in the stream says where the picture ends: a few rows of it at the
// bottom are put up with, and no columns.
[[nodiscard]] bool paddingIsAcceptable(const QSize &size, const QSize &coded);

// The codecs the user lets video be sent with: every one, unless the settings window turned
// it off (`video/send_codecs_off`, by name). The server chooses among those that are offered,
// so what is turned off here is never chosen.
[[nodiscard]] bool sendCodecAllowed(Discord::Voice::VideoCodec codec);
void setSendCodecAllowed(Discord::Voice::VideoCodec codec, bool allowed);
[[nodiscard]] QList<Discord::Voice::VideoCodec> allowedSendCodecs(const QList<Discord::Voice::VideoCodec> &encodable);

// What a screen or window share whose pictures are `size` offers to send with: the codecs this
// machine encodes, without AV1 when `av1Encoder` would add more to that size than is acceptable.
[[nodiscard]] QList<Discord::Voice::VideoCodec> shareCodecs(const QList<Discord::Voice::VideoCodec> &encodable, const QString &av1Encoder, const QSize &size);
// The same for the camera, whose size follows what its viewers want and cannot be told
// beforehand: AV1 only from an encoder that codes every size as it is.
[[nodiscard]] QList<Discord::Voice::VideoCodec> cameraCodecs(const QList<Discord::Voice::VideoCodec> &encodable, const QString &av1Encoder);

// Makes of what an encoder hands over the frame the RTP sender is promised, whichever encoder
// it was and however it was asked. H264 and H265 are Annex B, and a key frame opens with its
// parameter sets: SPS and PPS, after the VPS for H265. AV1 is a temporal unit of OBUs that
// each say their size, with the sequence header ahead of the picture in every key frame; a
// temporal delimiter may come first. VP8 is a bare frame. Whether a frame is a key frame is
// read from the frame: an IDR picture in H264, any IRAP picture in H265, a key frame that is
// shown in AV1. Every H264 SPS that leaves here, in a frame or kept for a later one, is one the
// official client does not rewrite on receiving it, since it could not then decrypt the frame
// (H264Sps.hpp).
class EncodedFrameFinisher
{
public:
    explicit EncodedFrameFinisher(Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown) : codec(codec) {}

    // An encoder's extradata, where its parameter sets may be before any frame has them.
    void rememberParameterSets(const uint8_t *extradata, size_t size);
    // Sets `keyframe`, and gives a key frame the parameter sets it came without, from the last
    // ones seen. False for a frame that cannot be made whole: one that is not of the codec's
    // making, or a key frame whose parameter sets have never been seen.
    [[nodiscard]] bool finish(Discord::Voice::EncodedVideoFrame &frame);

private:
    [[nodiscard]] bool finishAnnexB(Discord::Voice::EncodedVideoFrame &frame);
    [[nodiscard]] bool finishAv1(Discord::Voice::EncodedVideoFrame &frame);

    Discord::Voice::VideoCodec codec;
    // VPS, SPS and PPS, of which H264 has the last two.
    std::array<std::vector<uint8_t>, 3> parameterSets;
    // AV1's, without its OBU header.
    std::vector<uint8_t> sequenceHeader;
};

// Encodes live video with as little delay as the encoder allows: a picture goes in and its
// encoded frame comes out of the same call. It takes the best encoder that works, the graphics
// card's before a software one, and moves on to the next when one that worked stops working.
//
// Encoded frames are whole access units, as EncodedFrameFinisher leaves them, of pictures the
// size of the ones given. The exception is AV1 from a graphics card that codes it only in
// whole blocks, as a Radeon of the 7000 series does in ones of 64 by 16: the frames are of a
// picture that many rows and columns larger at most, black where the picture given ends, and
// nothing in them says where that is. codedSize() tells beforehand, and it is for whoever
// feeds the encoder to keep such pictures from it. Not thread safe; it belongs to the thread
// that feeds it.
class VideoEncoder
{
public:
    // What the pictures show, for the encoders that are tuned differently for the two.
    enum class Content {
        Screen,
        Camera,
    };

    struct Settings
    {
        Discord::Voice::VideoCodec codec = Discord::Voice::VideoCodec::Unknown;
        int framerate = 30;
        // Bits per second.
        int bitrate = 0;
        // Zero makes key frames only when asked for.
        int keyframeIntervalMs = 0;
        Content content = Content::Screen;
        // One of videoEncoderNames(), to use that encoder or none. Empty takes the best that works.
        QString encoderName;
    };

    enum class Outcome {
        Encoded,
        // The picture was on the graphics card and the encoder cannot take it from there. Nothing
        // is held against the encoder: the same picture in system memory is to be sent instead.
        CardFramesRefused,
        // No encoder is left that works.
        Failed,
    };

    struct Stats
    {
        // The encoder in use, with the hardware device in brackets when pictures reach it on the card.
        QString encoderName;
        bool hardware = false;
        int width = 0;
        int height = 0;
        quint64 encoded = 0;
        quint64 opened = 0;
        double averageEncodeMs = 0;
    };

    explicit VideoEncoder(const Settings &settings);
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder &) = delete;
    VideoEncoder &operator=(const VideoEncoder &) = delete;

    [[nodiscard]] bool hasEncoder() const { return candidate != nullptr; }
    // What the encoder the next picture goes to would code a picture of `size` as.
    [[nodiscard]] QSize codedSize(const QSize &size) const;
    // Whether the encoder the next picture goes to takes NV12 pictures as frames on the graphics
    // card (Direct3D 11's on Windows; elsewhere VAAPI's, and CUDA's for NVIDIA's encoders), and
    // whether it takes the screen's own BGRA ones as well, converting them itself to the
    // colours the frame is marked with.
    [[nodiscard]] bool takesCardFrames() const;
    [[nodiscard]] bool takesRgbCardFrames() const;
    // The format (an AVPixelFormat) pictures in system memory must have.
    [[nodiscard]] int softwareFormat() const;

    // The next picture becomes a key frame.
    void requestKeyframe();
    // Followed while running by the encoders that can. The others start over with the next
    // picture, which is then a key frame, and so not for every small move: they run a little
    // under a rate that has come down, where the next step down finds room, and are left
    // there until the rate has risen by enough to be worth the key frame. They never run
    // over the rate.
    void setBitrate(int bitrate);

    // Encodes one picture, whose size and format may differ from the last one's. `timestamp` is
    // in 90 kHz units, must grow, and comes back as the frame's RTP timestamp. The frame's own
    // timestamp and picture type are overwritten. Appends to `out`: one frame, or none when the
    // encoder's rate control dropped the picture.
    Outcome encode(AVFrame *frame, int64_t timestamp, std::vector<Discord::Voice::EncodedVideoFrame> &out);

    [[nodiscard]] const Stats &stats() const { return counters; }

    // An encoder that fails before it has made this many frames is given up on.
    static constexpr int TRIAL_FRAMES = 30;
    // How long a picture's frame is waited for before the encoder is taken to be running late.
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
    // For the streams of an encoder that cannot be told what colours its pictures are in.
    AVBSFContext *colourDescriber = nullptr;
    AVPacket *asEncoded = nullptr;

    bool cardFramesRefused = false;
    bool keyframeWanted = false;
    bool reopenWanted = false;
    // The rate last asked for when `settings.bitrate` was last chosen.
    int bitrateChosenFor = 0;
    int awaited = 0;
    quint64 framesSinceOpen = 0;
    int64_t lastKeyframeAt = 0;
    EncodedFrameFinisher finisher;

    Stats counters;
};

} // namespace Media
} // namespace Core
} // namespace Acheron
