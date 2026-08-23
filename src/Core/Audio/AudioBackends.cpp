#include "Core/Audio/AudioBackends.hpp"
#include "Core/Audio/Miniaudio.hpp"
#include "Core/Logging.hpp"

#include <QSettings>

namespace Acheron {
namespace Core {
namespace Audio {

namespace {

constexpr auto BackendKey = "audio/backend";

QList<ma_backend> enabledBackends()
{
    ma_backend backends[MA_BACKEND_COUNT];
    size_t count = 0;
    if (ma_get_enabled_backends(backends, MA_BACKEND_COUNT, &count) != MA_SUCCESS)
        return {};

    QList<ma_backend> result;
    for (size_t i = 0; i < count; i++) {
        if (backends[i] == ma_backend_custom || backends[i] == ma_backend_null)
            continue;
        result.append(backends[i]);
    }
    return result;
}

bool resolveBackend(const QString &name, ma_backend &out)
{
    if (name.isEmpty())
        return false;

    for (ma_backend backend : enabledBackends()) {
        if (name == QString::fromUtf8(ma_get_backend_name(backend))) {
            out = backend;
            return true;
        }
    }

    return false;
}

} // namespace

QStringList supportedAudioBackends()
{
    static const QStringList cached = []() {
        QStringList names;
        for (ma_backend backend : enabledBackends()) {
            ma_context context;
            if (ma_context_init(&backend, 1, nullptr, &context) != MA_SUCCESS)
                continue;
            ma_context_uninit(&context);
            names.append(QString::fromUtf8(ma_get_backend_name(backend)));
        }
        return names;
    }();

    return cached;
}

QString configuredAudioBackend()
{
    return QSettings().value(BackendKey).toString();
}

void setConfiguredAudioBackend(const QString &name)
{
    QSettings settings;
    if (name.isEmpty())
        settings.remove(BackendKey);
    else
        settings.setValue(BackendKey, name);
}

bool initAudioContext(ma_context *context, const ma_context_config *config)
{
    const QString name = configuredAudioBackend();

    ma_backend backend;
    if (resolveBackend(name, backend)) {
        if (ma_context_init(&backend, 1, config, context) == MA_SUCCESS)
            return true;
        qCWarning(LogMiniaudio) << "Audio backend" << name << "failed to initialize, falling back to the default";
    }

    return ma_context_init(nullptr, 0, config, context) == MA_SUCCESS;
}

bool initAudioDevice(const ma_device_config *config, ma_device *device)
{
    const QString name = configuredAudioBackend();

    ma_backend backend;
    if (resolveBackend(name, backend)) {
        if (ma_device_init_ex(&backend, 1, nullptr, config, device) == MA_SUCCESS)
            return true;
        qCWarning(LogMiniaudio) << "Audio backend" << name << "failed to initialize, falling back to the default";
    }

    return ma_device_init_ex(nullptr, 0, nullptr, config, device) == MA_SUCCESS;
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
