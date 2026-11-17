#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>

#include <opus.h>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Core {
namespace Audio {

struct VoiceSettings
{
    QByteArray inputDevice;
    QByteArray outputDevice;
    float inputGain = 1.0f;
    float outputVolume = 1.0f;
    int vadThreshold = 100;
    bool noiseSuppression = true;
    bool rnnoiseVad = true;
    int opusApplication = OPUS_APPLICATION_VOIP;
    int opusBitrateKbps = 64;
    int opusComplexity = 5;
    int opusSignalType = OPUS_SIGNAL_VOICE;
    bool opusFec = true;
    int opusPacketLossPercent = 0;
    bool opusDtx = false;
    int opusBitrateMode = BITRATE_MODE_CONSTRAINED_VBR;
    int opusMaxBandwidth = OPUS_BANDWIDTH_FULLBAND;
    int opusFrameDuration = OPUS_FRAMESIZE_ARG;
    int opusLsbDepth = 16;

    [[nodiscard]] static VoiceSettings load();
    void saveAndApply() const;

    static constexpr int MAX_VAD_THRESHOLD = 2000;
    static constexpr int BITRATE_MODE_VBR = 0;
    static constexpr int BITRATE_MODE_CONSTRAINED_VBR = 1;
    static constexpr int BITRATE_MODE_CBR = 2;
};

class VoiceSettingsNotifier : public QObject
{
    Q_OBJECT
public:
    [[nodiscard]] static VoiceSettingsNotifier *instance();

signals:
    void changed();

private:
    VoiceSettingsNotifier() = default;
};

[[nodiscard]] QHash<Snowflake, float> savedUserVolumes();
void saveUserVolume(Snowflake userId, float gain);
[[nodiscard]] QSet<Snowflake> savedUserMutes();
void saveUserMuted(Snowflake userId, bool muted);

} // namespace Audio
} // namespace Core
} // namespace Acheron
