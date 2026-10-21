#pragma once

#include <QSize>
#include <QtGlobal>

#include <array>
#include <optional>

#include "Core/Video/SendRateController.hpp"
#include "Discord/Voice/VideoFrame.hpp"

namespace Acheron {
namespace Core {
namespace Video {

// the official client's rules (docs/video-re/re-voice-gateway-video.md)
struct SendQuality
{
    static constexpr int SOURCE_HEIGHT = 0;

    int height = 720;
    int framerate = 30;

    [[nodiscard]] bool isSource() const { return height == SOURCE_HEIGHT; }
    [[nodiscard]] std::optional<int> heightLimit() const { return isSource() ? std::nullopt : std::optional<int>(height); }
    bool operator==(const SendQuality &other) const = default;
};

inline constexpr std::array<int, 5> SEND_HEIGHTS{ 480, 720, 1080, 1440, SendQuality::SOURCE_HEIGHT };
inline constexpr std::array<int, 3> SEND_FRAMERATES{ 15, 30, 60 };

struct SharerTier
{
    // `premium_type`: 1 Nitro Classic, 2 Nitro, 3 Nitro Basic
    int premiumType = 0;
    // `premium_tier`, 0 to 3; none in a private call
    int guildBoostTier = 0;
};

[[nodiscard]] bool mayUseQuality(const SendQuality &quality, const SharerTier &tier);
[[nodiscard]] SendQuality usableQuality(const SendQuality &wanted, const SharerTier &tier);
[[nodiscard]] SendQuality atHeight(const SendQuality &quality, int height, const SharerTier &tier);

[[nodiscard]] std::optional<QSize> announcedSize(const SendQuality &quality);

struct BitrateLimits
{
    int minBps = 0;
    int targetBps = 0;
    int maxBps = 0;

    bool operator==(const BitrateLimits &other) const = default;
};

[[nodiscard]] BitrateLimits bitrateLimits(const SendQuality &quality);

// zero `viewerPixels` means the server hasnt said. for a very small picture the maximum can fall under
// the minimum, as in the official client
[[nodiscard]] BitrateLimits limitsForViewerSize(const BitrateLimits &full, const SendQuality &quality, qint64 encodedPixels, int framerate, qint64 viewerPixels);

// taken up only when more than a twentieth away, so dragging a window doesnt drag the encoder along
class ViewerSize
{
public:
    bool offer(qint64 pixelCount);
    [[nodiscard]] qint64 pixelCount() const { return adopted; }

private:
    qint64 adopted = 0;
};

// `video/delay_estimate` off leaves out the delay estimate
[[nodiscard]] std::optional<qint64> estimatedCeiling(std::optional<quint64> receiverEstimate, std::optional<quint64> delayEstimate);

inline constexpr QSize CAMERA_CAPTURE_SIZE(1280, 720);
inline constexpr int CAMERA_FRAMERATE = 30;
inline constexpr int CAMERA_MIN_BITRATE = 150'000;
inline constexpr int CAMERA_MAX_BITRATE = 2'500'000;

// as the official client. off until tried against Discord (`video/camera_second_layer`)
[[nodiscard]] bool cameraSecondLayerEnabled();
void setCameraSecondLayerEnabled(bool enabled);
inline constexpr int SEND_CODEC_TIMEOUT_MS = 8000;

// as the official client, nothing is encoded while nobody receives it. the pipeline isnt promised to
// report a start while encoding nothing, so a layer waits for it before it is suspended
[[nodiscard]] inline bool encodingSuspends(bool pipelineStarted, bool videoAnnounced, std::optional<int> receivers)
{
    return pipelineStarted && (!videoAnnounced || receivers == 0);
}
inline constexpr int CAMERA_FULL_LAYER = 0;
inline constexpr int CAMERA_SECOND_LAYER = 1;
inline constexpr int CAMERA_LAYERS = 2;

struct CameraLayer
{
    int want = 0;
    QSize size;
    int maxBitrateBps = 0;

    bool operator==(const CameraLayer &other) const = default;
};

[[nodiscard]] int cameraMaxBitrate(qint64 pixels);
[[nodiscard]] CameraLayer cameraLayerFor(int want);
[[nodiscard]] int cameraFramerate(int want, bool muted);

struct CameraEncoding
{
    QSize size;
    int framerate = 0;
    int bitrateBps = 0;

    bool operator==(const CameraEncoding &other) const = default;
};

// muted means self-muted, or silent for SILENCE_BEFORE_MUTED_MS unless sharing a screen too. size and
// rate settle for SETTLE_MS, since each change costs a key frame and wants arrive in bursts
class CameraSendPlan
{
public:
    void start(qint64 nowMs);

    void setWant(int wanted);
    void setSelfMuted(bool muted);
    void setSpeaking(bool nowSpeaking, qint64 nowMs);
    void setSharingScreen(bool sharing);
    void reportLoss(std::optional<double> lossFraction, qint64 nowMs);
    void setReceiverLimit(std::optional<qint64> bitsPerSecond);

    CameraEncoding advance(qint64 nowMs);
    [[nodiscard]] std::optional<qint64> nextChangeIn(qint64 nowMs) const;

    [[nodiscard]] int servedWant() const { return wantInUse; }
    [[nodiscard]] int maxBitrate() const { return cameraLayerFor(wantInUse).maxBitrateBps; }
    [[nodiscard]] bool countsAsMuted(qint64 nowMs) const;

    static constexpr qint64 SETTLE_MS = 2000;
    static constexpr qint64 SILENCE_BEFORE_MUTED_MS = 15000;

private:
    [[nodiscard]] bool changeIsWaiting() const;

    int want = Discord::Voice::FULL_QUALITY;
    int wantInUse = Discord::Voice::FULL_QUALITY;
    bool selfMuted = false;
    bool speaking = false;
    bool sharingScreen = false;
    qint64 silentSinceMs = 0;
    SendRateController rate;
    int bitrateInUse = 0;
    qint64 changedAtMs = 0;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
