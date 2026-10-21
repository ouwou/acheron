#pragma once

#include <QByteArray>
#include <QSize>
#include <QtGlobal>

#include <windows.h>

#include <d3d11_1.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

template <typename T>
void drop(T *&object)
{
    if (object)
        object->Release();
    object = nullptr;
}

// the device's one drawing context, shared with every capture, encoder and decoder using the device
class DeviceLock
{
public:
    explicit DeviceLock(AVD3D11VADeviceContext *device) : device(device) { device->lock(device->lock_ctx); }
    ~DeviceLock() { device->unlock(device->lock_ctx); }

    DeviceLock(const DeviceLock &) = delete;
    DeviceLock &operator=(const DeviceLock &) = delete;

private:
    AVD3D11VADeviceContext *device;
};

// an empty adapter is the default one
[[nodiscard]] inline int openDirect3dDevice(AVBufferRef **device, const QByteArray &adapter)
{
    return av_hwdevice_ctx_create(device, AV_HWDEVICE_TYPE_D3D11VA, adapter.isEmpty() ? nullptr : adapter.constData(), nullptr, 0);
}

inline AVD3D11VADeviceContext *direct3d(AVBufferRef *device)
{
    return static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(device->data)->hwctx);
}

inline AVD3D11VADeviceContext *deviceOf(const AVFrame *card)
{
    return static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWFramesContext *>(card->hw_frames_ctx->data)->device_ctx->hwctx);
}

inline ID3D11Texture2D *textureOf(const AVFrame *card)
{
    return reinterpret_cast<ID3D11Texture2D *>(card->data[0]);
}

inline UINT sliceOf(const AVFrame *card)
{
    return static_cast<UINT>(reinterpret_cast<quintptr>(card->data[1]));
}

inline D3D11_VIDEO_PROCESSOR_CONTENT_DESC processorContent(const QSize &input, const QSize &output)
{
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = { 60, 1 };
    content.InputWidth = static_cast<UINT>(input.width());
    content.InputHeight = static_cast<UINT>(input.height());
    content.OutputFrameRate = { 60, 1 };
    content.OutputWidth = static_cast<UINT>(output.width());
    content.OutputHeight = static_cast<UINT>(output.height());
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    return content;
}

inline DXGI_COLOR_SPACE_TYPE yuvColourSpace(bool bt709, bool fullRange)
{
    if (bt709)
        return fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    return fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
}

// without this the processor converts with coefficients of the driver's choosing
inline void setColourSpaces(ID3D11VideoContext *context, ID3D11VideoProcessor *processor, DXGI_COLOR_SPACE_TYPE input, DXGI_COLOR_SPACE_TYPE output)
{
    // the older call leaves the RGB range to the driver, and drivers disagree
    ID3D11VideoContext1 *modern = nullptr;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11VideoContext1), reinterpret_cast<void **>(&modern)))) {
        modern->VideoProcessorSetStreamColorSpace1(processor, 0, input);
        modern->VideoProcessorSetOutputColorSpace1(processor, output);
        modern->Release();
        return;
    }

    const auto older = [](DXGI_COLOR_SPACE_TYPE type) {
        // all zero is full-range RGB
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE space = {};
        const bool bt709 = type == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 || type == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        const bool bt601 = type == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 || type == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
        if (bt709 || bt601) {
            space.YCbCr_Matrix = bt709 ? 1 : 0;
            const bool full = type == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 || type == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601;
            space.Nominal_Range = full ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        }
        return space;
    };
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE olderInput = older(input);
    const D3D11_VIDEO_PROCESSOR_COLOR_SPACE olderOutput = older(output);
    context->VideoProcessorSetStreamColorSpace(processor, 0, &olderInput);
    context->VideoProcessorSetOutputColorSpace(processor, &olderOutput);
}

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron
