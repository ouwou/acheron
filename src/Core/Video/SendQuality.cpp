#include "Core/Video/SendQuality.hpp"

#include <QSettings>

#include <algorithm>
#include <cmath>

namespace Acheron {
namespace Core {
namespace Video {

namespace {

constexpr int NITRO_CLASSIC = 1;
constexpr int NITRO = 2;

constexpr int MIN_BITRATE = 500'000;
constexpr int TARGET_BITRATE = 600'000;
constexpr int STANDARD_MAX_BITRATE = 3'500'000;
constexpr int HIGH_MAX_BITRATE = 9'000'000;
constexpr int BITRATE_FLOOR = 150'000;

constexpr int STANDARD_HEIGHT = 720;
constexpr int STANDARD_FRAMERATE = 30;
constexpr int SLOW_SOURCE_FRAMERATE = 10;

constexpr int PIXEL_FRAMES_PER_BIT = 50;

constexpr int CAMERA_MUTED_FRAMERATE = 20;
constexpr int CAMERA_MUTED_LOWEST_FRAMERATE = 12;

struct CameraRow
{
    int want;
    int width;
    int height;
};

constexpr std::array<CameraRow, 9> CAMERA_LADDER{ { { 100, 1280, 720 }, { 90, 896, 504 }, { 80, 672, 378 }, { 70, 640, 360 }, { 60, 512, 288 }, { 50, 416, 234 }, { 40, 384, 216 }, { 30, 320, 180 }, { 20, 256, 144 } } };

bool encodeAlike(int want, int other)
{
    return cameraLayerFor(want) == cameraLayerFor(other) && cameraFramerate(want, true) == cameraFramerate(other, true);
}

template <typename Values>
bool contains(const Values &values, int value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

int scaledUp(double share, int bitrate)
{
    return static_cast<int>(std::ceil(share * bitrate));
}

} // namespace

bool mayUseQuality(const SendQuality &quality, const SharerTier &tier)
{
    if (!contains(SEND_HEIGHTS, quality.height) || !contains(SEND_FRAMERATES, quality.framerate))
        return false;

    if (quality.isSource())
        return tier.premiumType == NITRO;

    const bool paysForIt = tier.premiumType == NITRO_CLASSIC || tier.premiumType == NITRO;
    if (quality.height > STANDARD_HEIGHT)
        return paysForIt || tier.guildBoostTier >= 2;
    if (quality.framerate > STANDARD_FRAMERATE)
        return paysForIt || tier.guildBoostTier >= 1;
    return true;
}

SendQuality usableQuality(const SendQuality &wanted, const SharerTier &tier)
{
    return mayUseQuality(wanted, tier) ? wanted : SendQuality{};
}

SendQuality atHeight(const SendQuality &quality, int height, const SharerTier &tier)
{
    SendQuality moved{ height, quality.framerate };
    if (!mayUseQuality(moved, tier))
        moved.framerate = STANDARD_FRAMERATE;
    return moved;
}

std::optional<qint64> estimatedCeiling(std::optional<quint64> receiverEstimate, std::optional<quint64> delayEstimate)
{
    if (!QSettings().value(QStringLiteral("video/delay_estimate"), true).toBool())
        delayEstimate.reset();
    if (receiverEstimate && delayEstimate)
        return static_cast<qint64>(std::min(*receiverEstimate, *delayEstimate));
    if (receiverEstimate || delayEstimate)
        return static_cast<qint64>(receiverEstimate ? *receiverEstimate : *delayEstimate);
    return std::nullopt;
}

bool cameraSecondLayerEnabled()
{
    return QSettings().value(QStringLiteral("video/camera_second_layer"), false).toBool();
}

void setCameraSecondLayerEnabled(bool enabled)
{
    QSettings().setValue(QStringLiteral("video/camera_second_layer"), enabled);
}

std::optional<QSize> announcedSize(const SendQuality &quality)
{
    if (quality.isSource())
        return std::nullopt;
    const int width = quality.height == 480 ? quality.height * 4 / 3 : quality.height * 16 / 9;
    return QSize(width, quality.height);
}

BitrateLimits bitrateLimits(const SendQuality &quality)
{
    const bool standard = quality.isSource() ? quality.framerate < SLOW_SOURCE_FRAMERATE : quality.height <= STANDARD_HEIGHT && quality.framerate <= STANDARD_FRAMERATE;
    return { MIN_BITRATE, TARGET_BITRATE, standard ? STANDARD_MAX_BITRATE : HIGH_MAX_BITRATE };
}

BitrateLimits limitsForViewerSize(const BitrateLimits &full, const SendQuality &quality, qint64 encodedPixels, int framerate, qint64 viewerPixels)
{
    if (quality.isSource() || viewerPixels <= 0 || viewerPixels >= encodedPixels)
        return full;

    const double share = static_cast<double>(viewerPixels) / static_cast<double>(encodedPixels);
    const qint64 leastMax = std::min<qint64>(encodedPixels * framerate / PIXEL_FRAMES_PER_BIT, full.maxBps);

    BitrateLimits scaled;
    scaled.minBps = std::max(scaledUp(share, full.minBps), BITRATE_FLOOR);
    scaled.targetBps = std::max(scaledUp(share, full.targetBps), BITRATE_FLOOR);
    scaled.maxBps = std::max(scaledUp(share, full.maxBps), static_cast<int>(leastMax));
    return scaled;
}

bool ViewerSize::offer(qint64 pixelCount)
{
    const qint64 difference = pixelCount > adopted ? pixelCount - adopted : adopted - pixelCount;
    if (difference * 20 <= adopted)
        return false;
    adopted = pixelCount;
    return true;
}

int cameraMaxBitrate(qint64 pixels)
{
    const qint64 fullPixels = static_cast<qint64>(CAMERA_CAPTURE_SIZE.width()) * CAMERA_CAPTURE_SIZE.height();
    return static_cast<int>(std::max<qint64>(CAMERA_MAX_BITRATE * pixels / fullPixels, CAMERA_MIN_BITRATE));
}

CameraLayer cameraLayerFor(int want)
{
    const auto row = std::find_if(CAMERA_LADDER.begin(), CAMERA_LADDER.end(), [want](const CameraRow &candidate) { return candidate.want <= want; });
    const CameraRow &chosen = row != CAMERA_LADDER.end() ? *row : CAMERA_LADDER.back();
    return { chosen.want, QSize(chosen.width, chosen.height), cameraMaxBitrate(static_cast<qint64>(chosen.width) * chosen.height) };
}

int cameraFramerate(int want, bool muted)
{
    if (!muted)
        return CAMERA_FRAMERATE;
    return want <= CAMERA_LADDER.back().want ? CAMERA_MUTED_LOWEST_FRAMERATE : CAMERA_MUTED_FRAMERATE;
}

void CameraSendPlan::start(qint64 nowMs)
{
    want = Discord::Voice::FULL_QUALITY;
    wantInUse = want;
    rate.restart(CAMERA_MIN_BITRATE, maxBitrate());
    bitrateInUse = rate.bitrate();
    changedAtMs = nowMs;
    silentSinceMs = nowMs;
}

void CameraSendPlan::setWant(int wanted)
{
    want = wanted;
    if (encodeAlike(want, wantInUse))
        wantInUse = want;
}

void CameraSendPlan::setSelfMuted(bool muted)
{
    selfMuted = muted;
}

void CameraSendPlan::setSpeaking(bool nowSpeaking, qint64 nowMs)
{
    if (speaking && !nowSpeaking)
        silentSinceMs = nowMs;
    speaking = nowSpeaking;
}

void CameraSendPlan::setSharingScreen(bool sharing)
{
    sharingScreen = sharing;
}

void CameraSendPlan::reportLoss(std::optional<double> lossFraction, qint64 nowMs)
{
    rate.report(lossFraction, nowMs);
}

void CameraSendPlan::setReceiverLimit(std::optional<qint64> bitsPerSecond)
{
    rate.setReceiverLimit(bitsPerSecond);
}

bool CameraSendPlan::countsAsMuted(qint64 nowMs) const
{
    if (sharingScreen)
        return false;
    return selfMuted || (!speaking && nowMs - silentSinceMs >= SILENCE_BEFORE_MUTED_MS);
}

bool CameraSendPlan::changeIsWaiting() const
{
    return wantInUse != want || bitrateInUse != rate.bitrate();
}

CameraEncoding CameraSendPlan::advance(qint64 nowMs)
{
    if (changeIsWaiting() && nowMs - changedAtMs >= SETTLE_MS) {
        if (wantInUse != want) {
            wantInUse = want;
            rate.setLimits(CAMERA_MIN_BITRATE, maxBitrate());
        }
        bitrateInUse = rate.bitrate();
        changedAtMs = nowMs;
    }
    return { cameraLayerFor(wantInUse).size, cameraFramerate(wantInUse, countsAsMuted(nowMs)), bitrateInUse };
}

std::optional<qint64> CameraSendPlan::nextChangeIn(qint64 nowMs) const
{
    std::optional<qint64> soonest;
    if (changeIsWaiting())
        soonest = std::max<qint64>(changedAtMs + SETTLE_MS - nowMs, 0);

    const bool silenceIsCounting = !sharingScreen && !selfMuted && !speaking && !countsAsMuted(nowMs);
    if (silenceIsCounting) {
        const qint64 untilMuted = silentSinceMs + SILENCE_BEFORE_MUTED_MS - nowMs;
        soonest = soonest ? std::min(*soonest, untilMuted) : untilMuted;
    }
    return soonest;
}

} // namespace Video
} // namespace Core
} // namespace Acheron
