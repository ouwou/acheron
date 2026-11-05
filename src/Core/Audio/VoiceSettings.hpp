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
    int inputGainPercent = 100;
    int outputVolumePercent = 100;
    int vadThreshold = 100;
    bool noiseSuppression = true;
    bool rnnoiseVad = true;
    int opusApplication = OPUS_APPLICATION_VOIP;
    int opusBitrateKbps = 64;
    int opusComplexity = 5;
    int opusSignalType = OPUS_SIGNAL_VOICE;
    bool opusFec = true;
    int opusPacketLossPercent = 0;

    [[nodiscard]] static VoiceSettings load();
    void saveAndApply() const;

    static constexpr int UNCHANGED_PERCENT = 100;
    static constexpr int MAX_GAIN_PERCENT = 200;
    static constexpr int MAX_VAD_THRESHOLD = 2000;
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

[[nodiscard]] QHash<Snowflake, int> savedUserVolumes();
void saveUserVolume(Snowflake userId, int percent);
[[nodiscard]] QSet<Snowflake> savedUserMutes();
void saveUserMuted(Snowflake userId, bool muted);

} // namespace Audio
} // namespace Core
} // namespace Acheron
