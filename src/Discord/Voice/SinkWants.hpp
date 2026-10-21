#pragma once

#include <QHash>
#include <QList>
#include <QMetaType>

#include "Core/Snowflake.hpp"
#include "VideoFrame.hpp"
#include "VoiceEntities.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

struct VideoLayer
{
    quint32 ssrc = 0;
    quint32 rtxSsrc = 0;
    int quality = FULL_QUALITY;

    bool operator==(const VideoLayer &other) const = default;
};

struct VideoSender
{
    Core::Snowflake userId;
    QList<VideoLayer> layers;
    bool wanted = true;
    int tilePixels = 0;
};

struct SinkWantsInput
{
    QList<VideoSender> senders;
    Core::Snowflake focusedUser;
    // where the official client also sizes its request to the tile
    bool oneToOne = false;
};

struct SinkWantsPlan
{
    MediaSinkWants wants;
    QHash<Core::Snowflake, quint32> deliver;
};

// the official client's ladder: 1280x720 worth of pixels shared between everyone, stepping down further
// for a tile smaller than the step
[[nodiscard]] int sinkWantLevel(int videoSenders, int tilePixels = 0);

// the official desktop client's rules
[[nodiscard]] SinkWantsPlan planSinkWants(const SinkWantsInput &input);

} // namespace Voice
} // namespace Discord
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Discord::Voice::VideoLayer)
