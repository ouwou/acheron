#include "VoiceSettings.hpp"

#include <QSettings>

namespace Acheron {
namespace Core {
namespace Audio {

namespace {

const char *const INPUT_DEVICE_KEY = "voice/input_device";
const char *const OUTPUT_DEVICE_KEY = "voice/output_device";
const char *const INPUT_GAIN_KEY = "voice/input_gain";
const char *const OUTPUT_VOLUME_KEY = "voice/output_volume";
const char *const VAD_THRESHOLD_KEY = "voice/vad_threshold";
const char *const NOISE_SUPPRESSION_KEY = "voice/noise_suppression";
const char *const RNNOISE_VAD_KEY = "voice/rnnoise_vad";
const char *const OPUS_APPLICATION_KEY = "voice/codec/application";
const char *const OPUS_BITRATE_KEY = "voice/codec/bitrate";
const char *const OPUS_COMPLEXITY_KEY = "voice/codec/complexity";
const char *const OPUS_SIGNAL_TYPE_KEY = "voice/codec/signal_type";
const char *const OPUS_FEC_KEY = "voice/codec/fec";
const char *const OPUS_PACKET_LOSS_KEY = "voice/codec/packet_loss";
const char *const USER_VOLUMES_GROUP = "voice/user_volumes";
const char *const USER_MUTES_GROUP = "voice/user_mutes";

} // namespace

VoiceSettings VoiceSettings::load()
{
    const QSettings stored;
    const VoiceSettings defaults;
    VoiceSettings settings;
    settings.inputDevice = stored.value(INPUT_DEVICE_KEY).toByteArray();
    settings.outputDevice = stored.value(OUTPUT_DEVICE_KEY).toByteArray();
    settings.inputGainPercent = stored.value(INPUT_GAIN_KEY, defaults.inputGainPercent).toInt();
    settings.outputVolumePercent = stored.value(OUTPUT_VOLUME_KEY, defaults.outputVolumePercent).toInt();
    settings.vadThreshold = stored.value(VAD_THRESHOLD_KEY, defaults.vadThreshold).toInt();
    settings.noiseSuppression = stored.value(NOISE_SUPPRESSION_KEY, defaults.noiseSuppression).toBool();
    settings.rnnoiseVad = stored.value(RNNOISE_VAD_KEY, defaults.rnnoiseVad).toBool();
    settings.opusApplication = stored.value(OPUS_APPLICATION_KEY, defaults.opusApplication).toInt();
    settings.opusBitrateKbps = stored.value(OPUS_BITRATE_KEY, defaults.opusBitrateKbps).toInt();
    settings.opusComplexity = stored.value(OPUS_COMPLEXITY_KEY, defaults.opusComplexity).toInt();
    settings.opusSignalType = stored.value(OPUS_SIGNAL_TYPE_KEY, defaults.opusSignalType).toInt();
    settings.opusFec = stored.value(OPUS_FEC_KEY, defaults.opusFec).toBool();
    settings.opusPacketLossPercent = stored.value(OPUS_PACKET_LOSS_KEY, defaults.opusPacketLossPercent).toInt();
    return settings;
}

void VoiceSettings::saveAndApply() const
{
    QSettings stored;
    stored.setValue(INPUT_DEVICE_KEY, inputDevice);
    stored.setValue(OUTPUT_DEVICE_KEY, outputDevice);
    stored.setValue(INPUT_GAIN_KEY, inputGainPercent);
    stored.setValue(OUTPUT_VOLUME_KEY, outputVolumePercent);
    stored.setValue(VAD_THRESHOLD_KEY, vadThreshold);
    stored.setValue(NOISE_SUPPRESSION_KEY, noiseSuppression);
    stored.setValue(RNNOISE_VAD_KEY, rnnoiseVad);
    stored.setValue(OPUS_APPLICATION_KEY, opusApplication);
    stored.setValue(OPUS_BITRATE_KEY, opusBitrateKbps);
    stored.setValue(OPUS_COMPLEXITY_KEY, opusComplexity);
    stored.setValue(OPUS_SIGNAL_TYPE_KEY, opusSignalType);
    stored.setValue(OPUS_FEC_KEY, opusFec);
    stored.setValue(OPUS_PACKET_LOSS_KEY, opusPacketLossPercent);
    VoiceSettingsNotifier *notifier = VoiceSettingsNotifier::instance();
    emit notifier->changed();
}

VoiceSettingsNotifier *VoiceSettingsNotifier::instance()
{
    static VoiceSettingsNotifier notifier;
    return &notifier;
}

QHash<Snowflake, int> savedUserVolumes()
{
    QSettings stored;
    stored.beginGroup(USER_VOLUMES_GROUP);
    QHash<Snowflake, int> volumes;
    for (const QString &key : stored.childKeys())
        volumes.insert(Snowflake(key.toULongLong()), stored.value(key).toInt());
    return volumes;
}

void saveUserVolume(Snowflake userId, int percent)
{
    QSettings stored;
    stored.beginGroup(USER_VOLUMES_GROUP);
    const QString key = QString::number(static_cast<quint64>(userId));
    if (percent == VoiceSettings::UNCHANGED_PERCENT)
        stored.remove(key);
    else
        stored.setValue(key, percent);
}

QSet<Snowflake> savedUserMutes()
{
    QSettings stored;
    stored.beginGroup(USER_MUTES_GROUP);
    QSet<Snowflake> muted;
    for (const QString &key : stored.childKeys())
        muted.insert(Snowflake(key.toULongLong()));
    return muted;
}

void saveUserMuted(Snowflake userId, bool muted)
{
    QSettings stored;
    stored.beginGroup(USER_MUTES_GROUP);
    const QString key = QString::number(static_cast<quint64>(userId));
    if (muted)
        stored.setValue(key, true);
    else
        stored.remove(key);
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
