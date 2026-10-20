#include "Core/Video/Windows/Monitors.hpp"

#if defined(Q_OS_WIN)

#include <dxgi.h>

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

namespace {

const QLatin1String SCREEN_ID_PREFIX("screen:");

} // namespace

std::vector<Monitor> attachedMonitors()
{
    using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
    static const auto createFactory = reinterpret_cast<CreateFactory>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1")));

    std::vector<Monitor> found;
    IDXGIFactory1 *factory = nullptr;
    if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return found;

    IDXGIAdapter *adapter = nullptr;
    for (UINT adapterIndex = 0; SUCCEEDED(factory->EnumAdapters(adapterIndex, &adapter)); adapterIndex++) {
        IDXGIOutput *output = nullptr;
        for (UINT outputIndex = 0; SUCCEEDED(adapter->EnumOutputs(outputIndex, &output)); outputIndex++) {
            DXGI_OUTPUT_DESC description;
            DEVMODEW mode = {};
            mode.dmSize = sizeof(mode);
            // The mode is in the screen's own pixels whatever the calling thread understands,
            // which the output's desktop coordinates are not.
            if (SUCCEEDED(output->GetDesc(&description)) && description.AttachedToDesktop && EnumDisplaySettingsW(description.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                Monitor monitor;
                monitor.deviceName = QString::fromWCharArray(description.DeviceName);
                monitor.adapter = static_cast<int>(adapterIndex);
                monitor.output = static_cast<int>(outputIndex);
                monitor.handle = description.Monitor;
                monitor.geometry = QRect(mode.dmPosition.x, mode.dmPosition.y, static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight));
                monitor.onItsSide = description.Rotation == DXGI_MODE_ROTATION_ROTATE90 || description.Rotation == DXGI_MODE_ROTATION_ROTATE270;
                monitor.turned = monitor.onItsSide || description.Rotation == DXGI_MODE_ROTATION_ROTATE180;
                found.push_back(monitor);
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

QString screenSourceId(const Monitor &monitor)
{
    return SCREEN_ID_PREFIX + monitor.deviceName;
}

const Monitor *monitorOf(const std::vector<Monitor> &monitors, const QString &sourceId)
{
    if (!sourceId.startsWith(SCREEN_ID_PREFIX))
        return nullptr;
    const QString deviceName = sourceId.mid(SCREEN_ID_PREFIX.size());
    for (const Monitor &monitor : monitors) {
        if (!deviceName.isEmpty() && monitor.deviceName == deviceName)
            return &monitor;
    }
    return nullptr;
}

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
