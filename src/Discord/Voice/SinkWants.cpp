#include "SinkWants.hpp"

#include <array>

namespace Acheron {
namespace Discord {
namespace Voice {

namespace {

struct LadderStep
{
    int want;
    int pixels;
};

constexpr std::array<LadderStep, 9> LADDER = { {
        { 20, 256 * 144 },
        { 30, 320 * 180 },
        { 40, 384 * 216 },
        { 50, 416 * 234 },
        { 60, 512 * 288 },
        { 70, 640 * 360 },
        { 80, 672 * 378 },
        { 90, 896 * 504 },
        { 100, 1280 * 720 },
} };

constexpr qint64 PIXEL_BUDGET = LADDER.back().pixels;
constexpr int FULL = 100;
constexpr int NONE = 0;

} // namespace

int sinkWantLevel(int videoSenders, int tilePixels)
{
    int level = LADDER.front().want;
    for (const LadderStep &step : LADDER) {
        if (tilePixels > 0 && tilePixels < step.pixels) {
            level = step.want;
            break;
        }
        if (static_cast<qint64>(step.pixels) * videoSenders > PIXEL_BUDGET)
            break;
        level = step.want;
    }
    return level;
}

SinkWantsPlan planSinkWants(const SinkWantsInput &input)
{
    SinkWantsPlan plan;

    int senderCount = 0;
    for (const VideoSender &sender : input.senders) {
        if (!sender.layers.isEmpty())
            senderCount++;
    }

    const int everyone = sinkWantLevel(senderCount);
    plan.wants.any = everyone;

    for (const VideoSender &sender : input.senders) {
        if (sender.layers.isEmpty())
            continue;

        if (!sender.wanted) {
            for (const VideoLayer &layer : sender.layers)
                plan.wants.bySsrc.insert(layer.ssrc, NONE);
            continue;
        }

        const int forTile = sinkWantLevel(senderCount, sender.tilePixels);
        const bool enlarged = sender.userId == input.focusedUser && everyone != FULL;

        quint32 taken = sender.layers.first().ssrc;
        if (sender.layers.size() > 1) {
            for (const VideoLayer &layer : sender.layers) {
                if (layer.quality == FULL_QUALITY) {
                    plan.wants.bySsrc.insert(layer.ssrc, enlarged ? FULL : NONE);
                    if (enlarged)
                        taken = layer.ssrc;
                } else if (enlarged) {
                    plan.wants.bySsrc.insert(layer.ssrc, NONE);
                } else {
                    // as the official client
                    if (input.oneToOne)
                        plan.wants.bySsrc.insert(layer.ssrc, forTile);
                    taken = layer.ssrc;
                }
            }
        } else if (enlarged) {
            plan.wants.bySsrc.insert(taken, FULL);
        } else if (input.oneToOne) {
            plan.wants.bySsrc.insert(taken, forTile);
        }

        plan.deliver.insert(sender.userId, taken);
    }

    return plan;
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
