#include "Core/Media/HardwareDiagnostics.hpp"

#include "Core/Logging.hpp"
#include "Core/Media/VideoDecoder.hpp"
#include "Core/Media/VideoEncoder.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSize>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
}

#ifdef Q_OS_WIN
#include <windows.h>
#include <d3d11.h>
#include <dbghelp.h>
#include <d3d9.h>
#include <dxgi.h>
#include <dxva2api.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/hwcontext_dxva2.h>
}
#endif

namespace Acheron {
namespace Core {
namespace Media {

namespace {

using Discord::Voice::VideoCodec;

#define DIAG qCInfo(LogVideo).noquote() << "[hwdiag]"

std::thread diagnosticsThread;
std::atomic<bool> stopRequested = false;

// ffmpeg's messages ---------------------------------------------------------------------------

// The same message over and over (a decoder complaining about every frame) is logged a few
// times and then counted.
constexpr int REPEATS_LOGGED = 20;

std::mutex ffmpegLogMutex;
QHash<QString, int> ffmpegRepeats;

int ffmpegLogLevel()
{
    const QByteArray asked = qgetenv("ACHERON_FFMPEG_LOG").toLower();
    if (asked == "quiet")
        return AV_LOG_QUIET;
    if (asked == "error")
        return AV_LOG_ERROR;
    if (asked == "warning")
        return AV_LOG_WARNING;
    if (asked == "info")
        return AV_LOG_INFO;
    if (asked == "debug")
        return AV_LOG_DEBUG;
    return AV_LOG_VERBOSE;
}

void ffmpegLog(void *object, int level, const char *format, va_list arguments)
{
    if (level > av_log_get_level())
        return;

    // ffmpeg hands lines over in pieces and says by this whether the next piece starts a line.
    thread_local int printPrefix = 1;
    thread_local QByteArray pending;
    char piece[1024];
    av_log_format_line2(object, level, format, arguments, piece, sizeof(piece), &printPrefix);
    pending += piece;
    if (!pending.endsWith('\n'))
        return;

    QString line = QString::fromUtf8(pending).trimmed();
    pending.clear();
    if (line.isEmpty())
        return;

    {
        // Each decoder has its own address in the line; the same message from the next one is
        // still the same message.
        static const QRegularExpression ADDRESS(QStringLiteral("@ [0-9a-fA-F]{8,16}\\]"));
        QString key = line;
        key.replace(ADDRESS, QStringLiteral("]"));
        std::lock_guard lock(ffmpegLogMutex);
        if (ffmpegRepeats.size() > 5000)
            ffmpegRepeats.clear();
        int &seen = ffmpegRepeats[key];
        seen++;
        if (seen > REPEATS_LOGGED) {
            if (seen % 1000 == 0)
                line = QStringLiteral("(seen %1 times) ").arg(seen) + line;
            else
                return;
        }
    }

    if (level <= AV_LOG_ERROR)
        qCWarning(LogVideo).noquote() << "[hwdiag] ffmpeg error:" << line;
    else if (level <= AV_LOG_WARNING)
        qCWarning(LogVideo).noquote() << "[hwdiag] ffmpeg warning:" << line;
    else
        qCInfo(LogVideo).noquote() << "[hwdiag] ffmpeg:" << line;
}

QString ffmpegError(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return QStringLiteral("%1 (%2)").arg(QString::fromUtf8(text)).arg(code);
}

// What ffmpeg was built with --------------------------------------------------------------------

void describeFfmpeg()
{
    DIAG << "ffmpeg" << av_version_info() << "avcodec" << AV_VERSION_MAJOR(avcodec_version()) << AV_VERSION_MINOR(avcodec_version()) << AV_VERSION_MICRO(avcodec_version());
    DIAG << "ffmpeg configuration:" << avcodec_configuration();

    QStringList types;
    for (AVHWDeviceType type = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE); type != AV_HWDEVICE_TYPE_NONE; type = av_hwdevice_iterate_types(type))
        types << QString::fromUtf8(av_hwdevice_get_type_name(type));
    DIAG << "ffmpeg hardware device types built in:" << types.join(QStringLiteral(", "));

    for (const char *name : { "h264", "hevc", "av1", "vp8", "vp9", "libdav1d", "libvpx" }) {
        const AVCodec *decoder = avcodec_find_decoder_by_name(name);
        if (!decoder) {
            DIAG << "decoder" << name << "not built in";
            continue;
        }
        QStringList configs;
        for (int i = 0;; i++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
            if (!config)
                break;
            configs << QStringLiteral("%1/%2").arg(QString::fromUtf8(av_hwdevice_get_type_name(config->device_type)), QString::fromUtf8(av_get_pix_fmt_name(config->pix_fmt)));
        }
        DIAG << "decoder" << name << "hardware configs:" << (configs.isEmpty() ? QStringLiteral("none") : configs.join(QStringLiteral(", ")));
    }

    QStringList encoders;
    QStringList missing;
    for (const char *name : { "h264_mf", "hevc_mf", "av1_mf", "h264_amf", "hevc_amf", "av1_amf", "h264_nvenc", "hevc_nvenc", "av1_nvenc", "h264_qsv", "hevc_qsv", "av1_qsv", "libvpx", "libx264", "libopenh264" })
        (avcodec_find_encoder_by_name(name) ? encoders : missing) << QString::fromLatin1(name);
    DIAG << "encoders built in:" << encoders.join(QStringLiteral(", ")) << "| not built in:" << missing.join(QStringLiteral(", "));

    const int flags = av_get_cpu_flags();
    QStringList cpu;
#if defined(Q_PROCESSOR_X86)
    if (flags & AV_CPU_FLAG_SSE2)
        cpu << QStringLiteral("SSE2");
    if (flags & AV_CPU_FLAG_SSSE3)
        cpu << QStringLiteral("SSSE3");
    if (flags & AV_CPU_FLAG_SSE4)
        cpu << QStringLiteral("SSE4.1");
    if (flags & AV_CPU_FLAG_AVX)
        cpu << QStringLiteral("AVX");
    if (flags & AV_CPU_FLAG_AVX2)
        cpu << QStringLiteral("AVX2");
#endif
    DIAG << "processor:" << QSysInfo::currentCpuArchitecture() << std::thread::hardware_concurrency() << "threads, ffmpeg sees" << cpu.join(QLatin1Char(' '));
}

#ifdef Q_OS_WIN

// Windows ----------------------------------------------------------------------------------

QString hresult(HRESULT result)
{
    return QStringLiteral("0x%1").arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
}

QString guidText(const GUID &guid)
{
    wchar_t text[64] = {};
    StringFromGUID2(guid, text, 64);
    return QString::fromWCharArray(text);
}

QString driverVersion(LARGE_INTEGER version)
{
    return QStringLiteral("%1.%2.%3.%4").arg(HIWORD(version.HighPart)).arg(LOWORD(version.HighPart)).arg(HIWORD(version.LowPart)).arg(LOWORD(version.LowPart));
}

template <typename Function>
Function loaded(const wchar_t *library, const char *name)
{
    HMODULE module = LoadLibraryW(library);
    return module ? reinterpret_cast<Function>(reinterpret_cast<void *>(GetProcAddress(module, name))) : nullptr;
}

void describeWindows()
{
    using RtlGetVersion = LONG(WINAPI *)(OSVERSIONINFOEXW *);
    OSVERSIONINFOEXW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    if (auto get = loaded<RtlGetVersion>(L"ntdll.dll", "RtlGetVersion"); get && get(&version) == 0)
        DIAG << "Windows" << version.dwMajorVersion << version.dwMinorVersion << "build" << version.dwBuildNumber << "service pack" << version.wServicePackMajor << "product type" << static_cast<int>(version.wProductType) << "|" << QSysInfo::prettyProductName();

    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
        DIAG << "memory:" << memory.ullTotalPhys / (1024 * 1024) << "MB, free" << memory.ullAvailPhys / (1024 * 1024) << "MB";

    DIAG << "executable:" << QCoreApplication::applicationFilePath() << "Qt" << qVersion();
}

void describeDxgiAdapters()
{
    using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
    auto create = loaded<CreateFactory>(L"dxgi.dll", "CreateDXGIFactory1");
    if (!create) {
        DIAG << "DXGI: CreateDXGIFactory1 not found";
        return;
    }
    IDXGIFactory1 *factory = nullptr;
    HRESULT result = create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory));
    if (FAILED(result)) {
        DIAG << "DXGI: CreateDXGIFactory1 failed" << hresult(result);
        return;
    }

    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        LARGE_INTEGER umd = {};
        const HRESULT versioned = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
        DIAG << QStringLiteral("DXGI adapter %1: \"%2\" vendor 0x%3 device 0x%4 subsys 0x%5 rev %6, %7 MB dedicated, %8 MB shared, flags %9, driver %10")
                        .arg(i)
                        .arg(QString::fromWCharArray(desc.Description))
                        .arg(desc.VendorId, 4, 16, QLatin1Char('0'))
                        .arg(desc.DeviceId, 4, 16, QLatin1Char('0'))
                        .arg(desc.SubSysId, 8, 16, QLatin1Char('0'))
                        .arg(desc.Revision)
                        .arg(desc.DedicatedVideoMemory / (1024 * 1024))
                        .arg(desc.SharedSystemMemory / (1024 * 1024))
                        .arg(desc.Flags)
                        .arg(SUCCEEDED(versioned) ? driverVersion(umd) : hresult(versioned));

        IDXGIOutput *output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            DXGI_OUTPUT_DESC outputDesc = {};
            output->GetDesc(&outputDesc);
            const RECT &r = outputDesc.DesktopCoordinates;
            DIAG << QStringLiteral("  output %1: %2 %3x%4 at %5,%6 rotation %7").arg(o).arg(QString::fromWCharArray(outputDesc.DeviceName)).arg(r.right - r.left).arg(r.bottom - r.top).arg(r.left).arg(r.top).arg(static_cast<int>(outputDesc.Rotation));
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
}

QString decoderProfileName(const GUID &guid)
{
    struct Known
    {
        GUID guid;
        const char *name;
    };
    static const Known KNOWN[] = {
        { { 0xee27417f, 0x5e28, 0x4e65, { 0xbe, 0xea, 0x1d, 0x26, 0xb5, 0x08, 0xad, 0xc9 } }, "MPEG2 VLD" },
        { { 0x1b81be68, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } }, "H264 VLD NoFGT" },
        { { 0x1b81be69, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } }, "H264 VLD FGT" },
        { { 0x604f8e68, 0x4951, 0x4c54, { 0x88, 0xfe, 0xab, 0xd2, 0x5c, 0x15, 0xb3, 0xd6 } }, "H264 VLD Intel ClearVideo" },
        { { 0x1b81bea3, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } }, "VC1 VLD" },
        { { 0x1b81bea4, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } }, "VC1 VLD 2010" },
        { { 0x5b11d51b, 0x2f4c, 0x4452, { 0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0 } }, "HEVC Main" },
        { { 0x107af0e0, 0xef1a, 0x4d19, { 0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13 } }, "HEVC Main10" },
        { { 0x463707f8, 0xa1d0, 0x4585, { 0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e } }, "VP9 Profile0" },
        { { 0xa4c749ef, 0x6ecf, 0x48aa, { 0x84, 0x48, 0x50, 0xa7, 0xa1, 0x16, 0x5f, 0xf7 } }, "VP9 10bit Profile2" },
        { { 0x90b899ea, 0x3a62, 0x4705, { 0x88, 0xb3, 0x8d, 0xf0, 0x4b, 0x27, 0x44, 0xe7 } }, "VP8 VLD" },
        { { 0xb8be4ccb, 0xcf53, 0x46ba, { 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a } }, "AV1 Profile0" },
    };
    for (const Known &known : KNOWN) {
        if (IsEqualGUID(known.guid, guid))
            return QString::fromLatin1(known.name);
    }
    return guidText(guid);
}

QString fourcc(DWORD format)
{
    if (format < 0x20202020)
        return QString::number(format);
    QString text;
    for (int i = 0; i < 4; i++)
        text += QChar(static_cast<char>((format >> (8 * i)) & 0xFF));
    return text;
}

void describeDirect3D9()
{
    using Create9 = IDirect3D9 *(WINAPI *)(UINT);
    auto create = loaded<Create9>(L"d3d9.dll", "Direct3DCreate9");
    if (IDirect3D9 *d3d = create ? create(D3D_SDK_VERSION) : nullptr) {
        for (UINT i = 0; i < d3d->GetAdapterCount(); i++) {
            D3DADAPTER_IDENTIFIER9 id = {};
            if (FAILED(d3d->GetAdapterIdentifier(i, 0, &id)))
                continue;
            DIAG << QStringLiteral("Direct3D 9 adapter %1: \"%2\" driver %3 version %4 vendor 0x%5 device 0x%6").arg(i).arg(QString::fromLocal8Bit(id.Description)).arg(QString::fromLocal8Bit(id.Driver)).arg(driverVersion(id.DriverVersion)).arg(id.VendorId, 4, 16, QLatin1Char('0')).arg(id.DeviceId, 4, 16, QLatin1Char('0'));
        }
        d3d->Release();
    } else {
        DIAG << "Direct3D 9: not available";
    }

    // The device ffmpeg's DXVA2 decoding makes, and what the driver says it decodes through it.
    AVBufferRef *device = nullptr;
    QElapsedTimer timer;
    timer.start();
    const int created = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_DXVA2, nullptr, nullptr, 0);
    if (created < 0) {
        DIAG << "DXVA2: ffmpeg could not make a device:" << ffmpegError(created);
        return;
    }
    DIAG << "DXVA2: ffmpeg made a device in" << timer.elapsed() << "ms";

    auto *dxva2 = static_cast<AVDXVA2DeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(device->data)->hwctx);
    HANDLE handle = nullptr;
    HRESULT result = dxva2->devmgr->OpenDeviceHandle(&handle);
    IDirectXVideoDecoderService *service = nullptr;
    if (SUCCEEDED(result))
        result = dxva2->devmgr->GetVideoService(handle, __uuidof(IDirectXVideoDecoderService), reinterpret_cast<void **>(&service));
    if (FAILED(result)) {
        DIAG << "DXVA2: no decoder service:" << hresult(result);
    } else {
        UINT count = 0;
        GUID *guids = nullptr;
        result = service->GetDecoderDeviceGuids(&count, &guids);
        DIAG << "DXVA2:" << (SUCCEEDED(result) ? QString::number(count) : hresult(result)) << "decoder profiles";
        for (UINT i = 0; SUCCEEDED(result) && i < count; i++) {
            UINT formatCount = 0;
            D3DFORMAT *formats = nullptr;
            QStringList targets;
            if (SUCCEEDED(service->GetDecoderRenderTargets(guids[i], &formatCount, &formats))) {
                for (UINT f = 0; f < formatCount; f++)
                    targets << fourcc(formats[f]);
                CoTaskMemFree(formats);
            }
            DIAG << "  DXVA2 profile" << decoderProfileName(guids[i]) << "->" << targets.join(QLatin1Char(' '));
        }
        CoTaskMemFree(guids);
        service->Release();
    }
    if (handle)
        dxva2->devmgr->CloseDeviceHandle(handle);
    av_buffer_unref(&device);
}

void describeDirect3D11()
{
    auto create = loaded<PFN_D3D11_CREATE_DEVICE>(L"d3d11.dll", "D3D11CreateDevice");
    if (!create) {
        DIAG << "Direct3D 11: d3d11.dll or D3D11CreateDevice not found";
        return;
    }
    for (const UINT flags : { 0u, static_cast<UINT>(D3D11_CREATE_DEVICE_VIDEO_SUPPORT) }) {
        ID3D11Device *device = nullptr;
        D3D_FEATURE_LEVEL level = {};
        const HRESULT result = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device, &level, nullptr);
        QString video;
        if (device) {
            ID3D11VideoDevice *videoDevice = nullptr;
            const HRESULT queried = device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void **>(&videoDevice));
            video = SUCCEEDED(queried) ? QStringLiteral("has ID3D11VideoDevice") : QStringLiteral("no ID3D11VideoDevice ") + hresult(queried);
            if (videoDevice)
                videoDevice->Release();
            device->Release();
        }
        DIAG << "Direct3D 11 device" << (flags ? "with VIDEO_SUPPORT:" : "plain:") << hresult(result) << "feature level" << QString::number(static_cast<int>(level), 16) << video;
    }

    AVBufferRef *device = nullptr;
    const int created = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    if (created < 0) {
        DIAG << "D3D11VA: ffmpeg could not make a device:" << ffmpegError(created) << "(expected before Windows 8)";
        return;
    }
    auto *d3d11 = static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(device->data)->hwctx);
    const UINT count = d3d11->video_device->GetVideoDecoderProfileCount();
    DIAG << "D3D11VA:" << count << "decoder profiles";
    for (UINT i = 0; i < count; i++) {
        GUID profile = {};
        if (FAILED(d3d11->video_device->GetVideoDecoderProfile(i, &profile)))
            continue;
        BOOL nv12 = FALSE;
        d3d11->video_device->CheckVideoDecoderFormat(&profile, DXGI_FORMAT_NV12, &nv12);
        DIAG << "  D3D11VA profile" << decoderProfileName(profile) << (nv12 ? "NV12" : "");
    }
    av_buffer_unref(&device);
}

void describeMediaFoundationEncoders()
{
    using EnumEx = HRESULT(WINAPI *)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *, const MFT_REGISTER_TYPE_INFO *, IMFActivate ***, UINT32 *);
    auto enumerate = loaded<EnumEx>(L"mfplat.dll", "MFTEnumEx");
    if (!enumerate) {
        DIAG << "Media Foundation: MFTEnumEx not found";
        return;
    }

    static const GUID VIDEO_ENCODER = { 0xf79eac7d, 0xe545, 0x4387, { 0xbd, 0xee, 0xd6, 0x47, 0xd7, 0xbd, 0xe4, 0x2a } };
    static const GUID VIDEO = { 0x73646976, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
    static const GUID FRIENDLY_NAME = { 0x314ffbae, 0x5b41, 0x4c95, { 0x9c, 0x19, 0x4e, 0x7d, 0x58, 0x6f, 0xac, 0xe3 } };
    static const GUID VENDOR_ID = { 0x3aecb0cc, 0x035b, 0x4bcc, { 0x81, 0x85, 0x2b, 0x8d, 0x55, 0x1e, 0xf3, 0xaf } };
    struct Format
    {
        const char *name;
        DWORD fourcc;
    };
    static const Format FORMATS[] = { { "H264", MAKEFOURCC('H', '2', '6', '4') }, { "HEVC", MAKEFOURCC('H', 'E', 'V', 'C') }, { "AV1", MAKEFOURCC('A', 'V', '0', '1') } };

    for (const Format &format : FORMATS) {
        MFT_REGISTER_TYPE_INFO output = { VIDEO, { format.fourcc, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } } };
        for (const UINT32 flags : { static_cast<UINT32>(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER), static_cast<UINT32>(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER) }) {
            IMFActivate **activates = nullptr;
            UINT32 count = 0;
            const HRESULT result = enumerate(VIDEO_ENCODER, flags, nullptr, &output, &activates, &count);
            QStringList names;
            for (UINT32 i = 0; i < count; i++) {
                wchar_t *name = nullptr;
                UINT32 length = 0;
                QString entry = activates[i]->GetAllocatedString(FRIENDLY_NAME, &name, &length) == S_OK ? QString::fromWCharArray(name) : QStringLiteral("?");
                CoTaskMemFree(name);
                wchar_t *vendor = nullptr;
                if (activates[i]->GetAllocatedString(VENDOR_ID, &vendor, &length) == S_OK)
                    entry += QStringLiteral(" [%1]").arg(QString::fromWCharArray(vendor));
                CoTaskMemFree(vendor);
                names << entry;
                activates[i]->Release();
            }
            CoTaskMemFree(activates);
            DIAG << "Media Foundation" << format.name << (flags & MFT_ENUM_FLAG_HARDWARE ? "hardware" : "software") << "encoders:" << (FAILED(result) ? hresult(result) : names.isEmpty() ? QStringLiteral("none")
                                                                                                                                                                                          : names.join(QStringLiteral("; ")));
        }
    }
}

void describeVendorRuntimes()
{
    using NvencMaxVersion = int(__stdcall *)(uint32_t *);
    if (auto get = loaded<NvencMaxVersion>(L"nvEncodeAPI64.dll", "NvEncodeAPIGetMaxSupportedVersion")) {
        uint32_t version = 0;
        const int status = get(&version);
        // ffmpeg in this build was made against NVENC API 11.1, which needs driver 471.41 or newer.
        DIAG << "NVENC: driver supports API" << QStringLiteral("%1.%2").arg(version >> 4).arg(version & 0xF) << "status" << status;
    } else {
        DIAG << "NVENC: nvEncodeAPI64.dll not loadable (no NVIDIA driver, or one without NVENC)";
    }
    DIAG << "CUDA driver nvcuda.dll:" << (LoadLibraryW(L"nvcuda.dll") ? "loads" : "not loadable");

    using AmfQueryVersion = int(__cdecl *)(uint64_t *);
    if (auto query = loaded<AmfQueryVersion>(L"amfrt64.dll", "AMFQueryVersion")) {
        uint64_t version = 0;
        const int status = query(&version);
        DIAG << "AMF: runtime" << QStringLiteral("%1.%2.%3.%4").arg(version >> 48).arg((version >> 32) & 0xFFFF).arg((version >> 16) & 0xFFFF).arg(version & 0xFFFF) << "status" << status;
    } else {
        DIAG << "AMF: amfrt64.dll not loadable (no AMD driver, or one without AMF)";
    }

    DIAG << "Intel Media SDK libmfxhw64.dll:" << (LoadLibraryW(L"libmfxhw64.dll") ? "loads" : "not loadable from the search path (the dispatcher looks it up in the registry)");
}

// Crash dumps -----------------------------------------------------------------------------------

// Where a crash leaves its dump, worked out before anything has gone wrong.
wchar_t crashDumpFolder[MAX_PATH] = {};

void writeCrashDump(EXCEPTION_POINTERS *exception)
{
    using WriteDump = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    auto write = loaded<WriteDump>(L"dbghelp.dll", "MiniDumpWriteDump");
    if (!write || !crashDumpFolder[0])
        return;

    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t path[MAX_PATH + 64];
    swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls\\acheron-crash-%04d%02d%02d-%02d%02d%02d.dmp", crashDumpFolder, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    MINIDUMP_EXCEPTION_INFORMATION information = {};
    information.ThreadId = GetCurrentThreadId();
    information.ExceptionPointers = exception;
    information.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithHandleData);
    write(GetCurrentProcess(), GetCurrentProcessId(), file, type, exception ? &information : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS *exception)
{
    writeCrashDump(exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

void onAbort(int)
{
    writeCrashDump(nullptr);
}

void installCrashDumps()
{
    const QString folder = QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (folder.isEmpty() || folder.size() >= MAX_PATH)
        return;
    folder.toWCharArray(crashDumpFolder);
    crashDumpFolder[folder.size()] = 0;
    SetUnhandledExceptionFilter(onUnhandledException);
    std::signal(SIGABRT, onAbort);
    DIAG << "a crash writes a dump into" << folder;
}

#endif // Q_OS_WIN

// Probes the app makes on its own when a call starts ----------------------------------------------

// Every encoder this build has, not only the first that works for each codec, as the app
// itself would stop at. A trial can take ten seconds and leave a thread behind, so only when
// the diagnostics are all the process is doing.
void tryEveryEncoder()
{
    for (VideoCodec codec : { VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 }) {
        for (const QString &name : videoEncoderNames(codec)) {
            if (stopRequested)
                return;
            DIAG << "encoder" << name << (videoEncoderWorks(name) ? "WORKS" : "does not work");
        }
    }
}

void runCodecProbes()
{
    QElapsedTimer timer;
    timer.start();
    QStringList decodable;
    for (VideoCodec codec : decodableVideoCodecs())
        decodable << Discord::Voice::videoCodecName(codec);
    DIAG << "decodable codecs:" << decodable.join(QStringLiteral(", ")) << "(" << timer.elapsed() << "ms )";

    timer.restart();
    QStringList encodable;
    for (VideoCodec codec : encodableVideoCodecs())
        encodable << Discord::Voice::videoCodecName(codec) + QStringLiteral(" by ") + videoEncoderFor(codec);
    DIAG << "encodable codecs:" << (encodable.isEmpty() ? QStringLiteral("none") : encodable.join(QStringLiteral(", "))) << "(" << timer.elapsed() << "ms of encoder trials )";
}

// Decoding the sample clips ---------------------------------------------------------------------

constexpr int THUMB_WIDTH = 32;
constexpr int THUMB_HEIGHT = 18;
using Thumbnail = std::array<uint8_t, THUMB_WIDTH * THUMB_HEIGHT>;

// Keeps a small copy of each picture's luma, which is laid out the same in every format a
// decoder hands over (planar YUV, NV12, P010 read as its high bytes).
class RecordingSink : public VideoFrameSink
{
public:
    void present(const AVFrame *frame) override
    {
        Thumbnail thumb = {};
        const bool sixteenBit = frame->format == AV_PIX_FMT_P010LE || frame->format == AV_PIX_FMT_YUV420P10LE;
        for (int y = 0; y < THUMB_HEIGHT; y++) {
            const uint8_t *row = frame->data[0] + static_cast<ptrdiff_t>(y * frame->height / THUMB_HEIGHT) * frame->linesize[0];
            for (int x = 0; x < THUMB_WIDTH; x++) {
                const int at = x * frame->width / THUMB_WIDTH;
                thumb[y * THUMB_WIDTH + x] = sixteenBit ? (frame->format == AV_PIX_FMT_P010LE ? row[at * 2 + 1] : static_cast<uint8_t>(reinterpret_cast<const uint16_t *>(row)[at] >> 2)) : row[at];
            }
        }
        std::lock_guard lock(mutex);
        thumbs.push_back(thumb);
        formats.insert(QString::fromUtf8(av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format))));
        size = QSize(frame->width, frame->height);
        arrived.notify_all();
    }

    bool waitFor(size_t count, int ms)
    {
        std::unique_lock lock(mutex);
        return arrived.wait_for(lock, std::chrono::milliseconds(ms), [&] { return thumbs.size() >= count; });
    }

    std::mutex mutex;
    std::condition_variable arrived;
    std::vector<Thumbnail> thumbs;
    QSet<QString> formats;
    QSize size;
};

struct Clip
{
    VideoCodec codec = VideoCodec::Unknown;
    std::vector<Discord::Voice::EncodedVideoFrame> frames;
    QString description;
};

Clip readClip(const QString &path)
{
    Clip clip;
    AVFormatContext *format = nullptr;
    int result = avformat_open_input(&format, QFile::encodeName(path).constData(), nullptr, nullptr);
    if (result < 0) {
        DIAG << "sample" << path << "does not open:" << ffmpegError(result);
        return clip;
    }
    avformat_find_stream_info(format, nullptr);
    const int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index < 0) {
        avformat_close_input(&format);
        return clip;
    }
    const AVCodecParameters *parameters = format->streams[index]->codecpar;
    switch (parameters->codec_id) {
    case AV_CODEC_ID_H264:
        clip.codec = VideoCodec::H264;
        break;
    case AV_CODEC_ID_HEVC:
        clip.codec = VideoCodec::H265;
        break;
    case AV_CODEC_ID_AV1:
        clip.codec = VideoCodec::AV1;
        break;
    case AV_CODEC_ID_VP8:
        clip.codec = VideoCodec::VP8;
        break;
    default:
        break;
    }
    clip.description = QStringLiteral("%1 %2x%3 profile %4 level %5 %6").arg(QString::fromUtf8(avcodec_get_name(parameters->codec_id))).arg(parameters->width).arg(parameters->height).arg(parameters->profile).arg(parameters->level).arg(QString::fromUtf8(av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters->format))));

    AVPacket *packet = av_packet_alloc();
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == index && packet->size > 0) {
            Discord::Voice::EncodedVideoFrame frame;
            frame.data.assign(packet->data, packet->data + packet->size);
            frame.keyframe = clip.frames.empty() || (packet->flags & AV_PKT_FLAG_KEY);
            frame.rtpTimestamp = static_cast<uint32_t>(clip.frames.size() * 3000);
            clip.frames.push_back(std::move(frame));
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&format);
    return clip;
}

struct DecodeRun
{
    VideoDecoder::Stats stats;
    std::vector<Thumbnail> thumbs;
    QStringList formats;
    QSize size;
    qint64 wallMs = 0;
};

// Frames go in no faster than they come out, the way a live stream's would, since the decoder
// gives up on a backlog that would never build up in a call.
DecodeRun decodeClip(const Clip &clip, bool hardware)
{
    auto sink = std::make_shared<RecordingSink>();
    DecodeRun run;
    QElapsedTimer timer;
    timer.start();
    {
        VideoDecoder decoder(hardware);
        decoder.setSink(sink);
        // A decoder that has stopped giving pictures is not waited for long on every frame.
        int patienceMs = 2000;
        for (size_t i = 0; i < clip.frames.size() && !stopRequested; i++) {
            decoder.push(clip.codec, clip.frames[i]);
            if (i >= 2 && !sink->waitFor(i - 1, patienceMs))
                patienceMs = 50;
        }
        sink->waitFor(clip.frames.size(), 2000);
        run.stats = decoder.stats();
    }
    run.wallMs = timer.elapsed();
    std::lock_guard lock(sink->mutex);
    run.thumbs = sink->thumbs;
    run.formats = QStringList(sink->formats.cbegin(), sink->formats.cend());
    run.size = sink->size;
    return run;
}

QString describeRun(const DecodeRun &run, size_t frames)
{
    return QStringLiteral("%1: %2/%3 pictures, %4 errors, %5 dropped, %6 ms per frame on average, %7 ms in all, %8x%9 %10")
            .arg(run.stats.decoderName.isEmpty() ? QStringLiteral("(no decoder)") : run.stats.decoderName)
            .arg(run.thumbs.size())
            .arg(frames)
            .arg(run.stats.errors)
            .arg(run.stats.dropped)
            .arg(run.stats.averageDecodeMs, 0, 'f', 2)
            .arg(run.wallMs)
            .arg(run.size.width())
            .arg(run.size.height())
            .arg(run.formats.join(QLatin1Char('/')));
}

void decodeSamples()
{
    const QDir folder(QCoreApplication::applicationDirPath() + QStringLiteral("/video-samples"));
    if (!folder.exists()) {
        DIAG << "no" << folder.absolutePath() << "folder, so no sample clips were decoded";
        return;
    }
    if (!hardwareDecodingEnabled())
        DIAG << "hardware decoding is turned off in the settings (video/hardware_decode); the samples are decoded with the card anyway";

    for (const QFileInfo &file : folder.entryInfoList(QDir::Files, QDir::Name)) {
        if (stopRequested)
            return;
        const Clip clip = readClip(file.absoluteFilePath());
        if (clip.codec == VideoCodec::Unknown || clip.frames.empty()) {
            DIAG << "sample" << file.fileName() << "skipped: no frames of a codec calls use";
            continue;
        }
        DIAG << "sample" << file.fileName() << ":" << clip.description << "," << clip.frames.size() << "frames";

        const DecodeRun software = decodeClip(clip, false);
        DIAG << "  software" << describeRun(software, clip.frames.size());
        const DecodeRun hardware = decodeClip(clip, true);
        DIAG << "  hardware" << describeRun(hardware, clip.frames.size());

        // Pictures that differ much from software's are wrong, even where nothing reported an error.
        const size_t compared = std::min(software.thumbs.size(), hardware.thumbs.size());
        double worst = 0;
        double total = 0;
        size_t worstAt = 0;
        for (size_t i = 0; i < compared; i++) {
            double difference = 0;
            for (size_t p = 0; p < software.thumbs[i].size(); p++)
                difference += std::abs(int(software.thumbs[i][p]) - int(hardware.thumbs[i][p]));
            difference /= software.thumbs[i].size();
            total += difference;
            if (difference > worst) {
                worst = difference;
                worstAt = i;
            }
        }
        if (compared == 0)
            DIAG << "  RESULT" << file.fileName() << ": nothing to compare";
        else
            DIAG << "  RESULT" << file.fileName() << ":" << (hardware.stats.hardware ? "decoded on the card" : "NOT decoded on the card") << ", luma differs from software by" << QString::number(total / compared, 'f', 2) << "on average and at most" << QString::number(worst, 'f', 2) << "(picture" << worstAt << ") of 255;" << (worst < 6 ? "matches" : "DIFFERS");
    }
}

void runDiagnostics(bool everyEncoder)
{
#ifdef Q_OS_WIN
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    DIAG << "---- hardware video diagnostics start ----";
    describeFfmpeg();
#ifdef Q_OS_WIN
    describeWindows();
    describeDxgiAdapters();
    describeDirect3D9();
    describeDirect3D11();
    describeMediaFoundationEncoders();
    describeVendorRuntimes();
#endif
    if (!stopRequested && everyEncoder)
        tryEveryEncoder();
    if (!stopRequested)
        runCodecProbes();
    if (!stopRequested)
        decodeSamples();
    DIAG << "---- hardware video diagnostics end" << (stopRequested ? "(stopped early)" : "") << "----";
#ifdef Q_OS_WIN
    CoUninitialize();
#endif
}

void installLoggingAndCrashDumps()
{
    av_log_set_level(ffmpegLogLevel());
    av_log_set_callback(ffmpegLog);
#ifdef Q_OS_WIN
    installCrashDumps();
#endif
}

} // namespace

void startHardwareDiagnostics()
{
    installLoggingAndCrashDumps();
    if (qEnvironmentVariableIsSet("ACHERON_NO_HWDIAG"))
        return;
    diagnosticsThread = std::thread(runDiagnostics, false);
}

void finishHardwareDiagnostics()
{
    stopRequested = true;
    if (diagnosticsThread.joinable())
        diagnosticsThread.join();
    av_log_set_callback(av_log_default_callback);
}

bool onlyDiagnosticsWereAskedFor(const QStringList &arguments)
{
    return arguments.contains(QStringLiteral("--hwdiag-only"));
}

void runOnlyDiagnosticsAndExit()
{
    installLoggingAndCrashDumps();
    // On a thread of its own for the same COM apartment as in the app.
    std::thread(runDiagnostics, true).join();
    av_log_set_callback(av_log_default_callback);
    Logger::cleanup();
#ifdef Q_OS_WIN
    TerminateProcess(GetCurrentProcess(), 0);
#endif
    std::_Exit(0);
}

} // namespace Media
} // namespace Core
} // namespace Acheron
