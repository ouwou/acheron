#pragma once

#include <QString>
#include <QStringList>

struct ma_context;
struct ma_context_config;
struct ma_device;
struct ma_device_config;

namespace Acheron {
namespace Core {
namespace Audio {

QStringList supportedAudioBackends();

QString configuredAudioBackend();
void setConfiguredAudioBackend(const QString &name);

bool initAudioContext(ma_context *context, const ma_context_config *config);
bool initAudioDevice(const ma_device_config *config, ma_device *device);

} // namespace Audio
} // namespace Core
} // namespace Acheron
