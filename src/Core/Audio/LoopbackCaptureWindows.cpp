#include "Core/Audio/LoopbackCapture.hpp"

#ifdef Q_OS_WIN

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Audio/PlayedSoundRemover.hpp"
#include "Core/Logging.hpp"

#include <QSettings>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace Acheron {
namespace Core {
namespace Audio {

namespace {

using Target = LoopbackCapture::Target;

constexpr DWORD ACTIVATION_TIMEOUT_MS = 3000;
// 100 ns units
constexpr REFERENCE_TIME BUFFER_DURATION = 200 * 10000;
// Windows is silent while nothing plays
constexpr DWORD SOUND_STOPPED_MS = 50;
constexpr int BYTES_PER_SAMPLE_FRAME = AUDIO_CHANNELS * AUDIO_SAMPLE_BYTES;
// Windows before 10 never signals a loopback capture's event, so it is polled
constexpr DWORD DEVICE_POLL_MS = 10;
const char *const WHOLE_DEVICE_KEY = "audio/stream_sound_from_whole_device";

struct Released
{
    void operator()(IUnknown *object) const { object->Release(); }
};
template <typename T>
using ComPointer = std::unique_ptr<T, Released>;

// called by Windows on its own thread. nothing promises Windows is done reading the request when the
// call returns, so the handler owns it
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler
{
public:
    explicit ActivationHandler(const Target &target)
    {
        loopback.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        loopback.ProcessLoopbackParams.TargetProcessId = target.processId;
        loopback.ProcessLoopbackParams.ProcessLoopbackMode = target.scope == LoopbackCapture::Scope::ProcessTree ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
        asked.vt = VT_BLOB;
        asked.blob.cbSize = sizeof(loopback);
        asked.blob.pBlobData = reinterpret_cast<BYTE *>(&loopback);
    }

    [[nodiscard]] PROPVARIANT *parameters() { return &asked; }
    [[nodiscard]] HANDLE completed() const { return done; }

    // Windows refuses a handler that isnt IAgileObject
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **object) override
    {
        if (id != __uuidof(IUnknown) && id != __uuidof(IActivateAudioInterfaceCompletionHandler) && id != __uuidof(IAgileObject)) {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        *object = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --references;
        if (left == 0)
            delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *) override
    {
        SetEvent(done);
        return S_OK;
    }

private:
    ~ActivationHandler() { CloseHandle(done); }

    std::atomic<ULONG> references{ 1 };
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    AUDIOCLIENT_ACTIVATION_PARAMS loopback = {};
    PROPVARIANT asked = {};
};

struct PendingActivation
{
    ComPointer<ActivationHandler> handler;
    ComPointer<IActivateAudioInterfaceAsyncOperation> operation;
};

HRESULT beginActivation(const Target &target, PendingActivation &pending)
{
    // absent on Windows 7; a missing import stops the program at load
    using ActivateAsync = HRESULT(WINAPI *)(LPCWSTR, REFIID, PROPVARIANT *, IActivateAudioInterfaceCompletionHandler *, IActivateAudioInterfaceAsyncOperation **);
    static const auto activateAsync = reinterpret_cast<ActivateAsync>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"mmdevapi.dll"), "ActivateAudioInterfaceAsync")));
    if (!activateAsync)
        return E_NOTIMPL;

    pending.handler.reset(new ActivationHandler(target));
    IActivateAudioInterfaceAsyncOperation *operation = nullptr;
    const HRESULT result = activateAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), pending.handler->parameters(), pending.handler.get(), &operation);
    pending.operation.reset(operation);
    return result;
}

HRESULT activatedClient(const PendingActivation &pending, ComPointer<IAudioClient> &client)
{
    HRESULT activated = E_FAIL;
    IUnknown *made = nullptr;
    const HRESULT answered = pending.operation->GetActivateResult(&activated, &made);
    if (FAILED(answered))
        return answered;
    if (FAILED(activated) || !made)
        return FAILED(activated) ? activated : E_NOINTERFACE;

    IAudioClient *audioClient = nullptr;
    const HRESULT result = made->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&audioClient));
    made->Release();
    client.reset(audioClient);
    return result;
}

QString failureText(const char *step, HRESULT result)
{
    return LoopbackCapture::tr("Windows could not capture the sound (%1, error 0x%2).").arg(QLatin1String(step)).arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
}

struct FreedMixFormat
{
    void operator()(WAVEFORMATEX *format) const { CoTaskMemFree(format); }
};

struct FreedResampler
{
    void operator()(SwrContext *resampler) const { swr_free(&resampler); }
};

AVSampleFormat sampleFormatOf(const WAVEFORMATEX &format)
{
    const bool extensible = format.wFormatTag == WAVE_FORMAT_EXTENSIBLE;
    // the subformat GUIDs differ from each other only in their first field, which is the format tag
    const DWORD tag = extensible ? reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format).SubFormat.Data1 : format.wFormatTag;
    if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32)
        return AV_SAMPLE_FMT_FLT;
    if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16)
        return AV_SAMPLE_FMT_S16;
    if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 32)
        return AV_SAMPLE_FMT_S32;
    return AV_SAMPLE_FMT_NONE;
}

// the device mixes in its own rate and speakers; the stream takes 48 kHz stereo
std::unique_ptr<SwrContext, FreedResampler> resamplerFrom(const WAVEFORMATEX &format, AVSampleFormat sampleFormat)
{
    AVChannelLayout from = {};
    const DWORD mask = format.wFormatTag == WAVE_FORMAT_EXTENSIBLE ? reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format).dwChannelMask : 0;
    if (mask == 0 || av_channel_layout_from_mask(&from, mask) < 0 || from.nb_channels != format.nChannels)
        av_channel_layout_default(&from, format.nChannels);
    AVChannelLayout to = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext *resampler = nullptr;
    const int made = swr_alloc_set_opts2(&resampler, &to, AV_SAMPLE_FMT_FLT, AUDIO_SAMPLE_RATE, &from, sampleFormat, static_cast<int>(format.nSamplesPerSec), 0, nullptr);
    av_channel_layout_uninit(&from);
    if (made < 0 || swr_init(resampler) < 0)
        swr_free(&resampler);
    return std::unique_ptr<SwrContext, FreedResampler>(resampler);
}

} // namespace

class LoopbackCapture::Worker
{
public:
    explicit Worker(LoopbackCapture *owner) : owner(owner) {}

    ~Worker()
    {
        {
            const std::lock_guard lock(mutex);
            quitting = true;
        }
        SetEvent(asked);
        if (thread.joinable())
            thread.join();
        CloseHandle(asked);
        CloseHandle(soundArrived);
    }

    void request(const std::optional<Target> &target)
    {
        {
            const std::lock_guard lock(mutex);
            wanted = target;
            requests++;
            if (target && !thread.joinable())
                thread = std::thread([this] { run(); });
        }
        SetEvent(asked);
    }

private:
    void run()
    {
        const bool comStarted = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

        quint64 served = 0;
        for (;;) {
            std::optional<Target> target;
            quint64 request = 0;
            {
                const std::lock_guard lock(mutex);
                if (quitting)
                    break;
                request = requests;
                if (request != served)
                    target = wanted;
            }
            served = request;
            if (!target) {
                WaitForSingleObject(asked, INFINITE);
                continue;
            }

            const QString failure = target->wholeDevice ? captureWholeDevice(request) : capture(*target, request);
            if (failure.isEmpty() || superseded(request))
                continue;
            qCWarning(LogVoice) << "Sound capture of process" << target->processId << "failed:" << failure;
            emit owner->failed(failure);
        }

        if (comStarted)
            CoUninitialize();
    }

    QString capture(const Target &target, quint64 request)
    {
        PendingActivation activation;
        HRESULT result = beginActivation(target, activation);
        if (FAILED(result))
            return failureText("ActivateAudioInterfaceAsync", result);

        for (;;) {
            const HANDLE awaited[] = { activation.handler->completed(), asked };
            const DWORD woken = WaitForMultipleObjects(2, awaited, FALSE, ACTIVATION_TIMEOUT_MS);
            if (woken == WAIT_OBJECT_0)
                break;
            if (woken != WAIT_OBJECT_0 + 1)
                return failureText("ActivateAudioInterfaceAsync", HRESULT_FROM_WIN32(woken == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError()));
            if (superseded(request))
                return {};
        }

        ComPointer<IAudioClient> client;
        result = activatedClient(activation, client);
        if (FAILED(result))
            return failureText("GetActivateResult", result);

        // a process loopback client has no mix format; it converts to what it is told
        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = AUDIO_CHANNELS;
        format.nSamplesPerSec = AUDIO_SAMPLE_RATE;
        format.wBitsPerSample = AUDIO_SAMPLE_BYTES * 8;
        format.nBlockAlign = BYTES_PER_SAMPLE_FRAME;
        format.nAvgBytesPerSec = AUDIO_SAMPLE_RATE * BYTES_PER_SAMPLE_FRAME;
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, BUFFER_DURATION, 0, &format, nullptr);
        if (FAILED(result))
            return failureText("IAudioClient::Initialize", result);

        ResetEvent(soundArrived);
        result = client->SetEventHandle(soundArrived);
        if (FAILED(result))
            return failureText("IAudioClient::SetEventHandle", result);

        IAudioCaptureClient *service = nullptr;
        result = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&service));
        if (FAILED(result))
            return failureText("IAudioClient::GetService", result);
        const ComPointer<IAudioCaptureClient> reader(service);

        result = client->Start();
        if (FAILED(result))
            return failureText("IAudioClient::Start", result);

        QByteArray unsent;
        unsent.reserve(AUDIO_FRAME_SIZE * 2);
        QString failure;
        for (;;) {
            const HANDLE awaited[] = { asked, soundArrived };
            const DWORD woken = WaitForMultipleObjects(2, awaited, FALSE, unsent.isEmpty() ? INFINITE : SOUND_STOPPED_MS);
            if (woken == WAIT_OBJECT_0) {
                if (superseded(request))
                    break;
                continue;
            }
            if (woken == WAIT_TIMEOUT) {
                unsent.append(AUDIO_FRAME_SIZE - unsent.size(), '\0');
                emit owner->frameCaptured(unsent);
                unsent.clear();
                continue;
            }
            if (woken != WAIT_OBJECT_0 + 1) {
                failure = failureText("WaitForMultipleObjects", HRESULT_FROM_WIN32(GetLastError()));
                break;
            }

            result = read(reader.get(), unsent);
            if (FAILED(result)) {
                failure = failureText("IAudioCaptureClient::GetBuffer", result);
                break;
            }
        }

        client->Stop();
        return failure;
    }

    // Windows before 10 cannot leave this program out of a capture, so the whole output device is taken
    // and what this program played is removed from it
    QString captureWholeDevice(quint64 request)
    {
        IMMDeviceEnumerator *madeEnumerator = nullptr;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&madeEnumerator));
        if (FAILED(result))
            return failureText("CoCreateInstance", result);
        const ComPointer<IMMDeviceEnumerator> enumerator(madeEnumerator);

        IMMDevice *madeDevice = nullptr;
        result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &madeDevice);
        if (FAILED(result))
            return failureText("GetDefaultAudioEndpoint", result);
        const ComPointer<IMMDevice> device(madeDevice);

        IAudioClient *madeClient = nullptr;
        result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&madeClient));
        if (FAILED(result))
            return failureText("IMMDevice::Activate", result);
        const ComPointer<IAudioClient> client(madeClient);

        WAVEFORMATEX *madeFormat = nullptr;
        result = client->GetMixFormat(&madeFormat);
        if (FAILED(result))
            return failureText("IAudioClient::GetMixFormat", result);
        const std::unique_ptr<WAVEFORMATEX, FreedMixFormat> format(madeFormat);
        const AVSampleFormat sampleFormat = sampleFormatOf(*format);
        const auto resampler = sampleFormat == AV_SAMPLE_FMT_NONE ? nullptr : resamplerFrom(*format, sampleFormat);
        if (!resampler)
            return LoopbackCapture::tr("Windows mixes sound in a format this program cannot read (%1 bits, tag %2).").arg(format->wBitsPerSample).arg(format->wFormatTag);

        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, BUFFER_DURATION, 0, format.get(), nullptr);
        if (FAILED(result))
            return failureText("IAudioClient::Initialize", result);

        IAudioCaptureClient *service = nullptr;
        result = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&service));
        if (FAILED(result))
            return failureText("IAudioClient::GetService", result);
        const ComPointer<IAudioCaptureClient> reader(service);

        result = client->Start();
        if (FAILED(result))
            return failureText("IAudioClient::Start", result);
        qCInfo(LogVoice) << "Capturing the whole output device at" << format->nSamplesPerSec << "Hz," << format->nChannels << "channels, taking this program's own sound out";

        DeviceReader deviceReader(owner, resampler.get(), static_cast<int>(format->nBlockAlign));
        QString failure;
        DWORD quietFor = 0;
        for (;;) {
            if (WaitForSingleObject(asked, DEVICE_POLL_MS) == WAIT_OBJECT_0 && superseded(request))
                break;
            bool heard = false;
            result = deviceReader.read(reader.get(), heard);
            if (FAILED(result)) {
                failure = failureText("IAudioCaptureClient::GetBuffer", result);
                break;
            }
            quietFor = heard ? 0 : quietFor + DEVICE_POLL_MS;
            if (quietFor >= SOUND_STOPPED_MS)
                deviceReader.flush();
        }

        client->Stop();
        return failure;
    }

    class DeviceReader
    {
    public:
        DeviceReader(LoopbackCapture *owner, SwrContext *resampler, int bytesPerFrame) : owner(owner), resampler(resampler), bytesPerFrame(bytesPerFrame) {}

        HRESULT read(IAudioCaptureClient *reader, bool &heard)
        {
            for (;;) {
                UINT32 waiting = 0;
                HRESULT result = reader->GetNextPacketSize(&waiting);
                if (FAILED(result) || waiting == 0)
                    return result;

                BYTE *samples = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 capturedAt = 0;
                result = reader->GetBuffer(&samples, &frames, &flags, nullptr, &capturedAt);
                if (FAILED(result))
                    return result;
                heard = true;
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                    remover.startOver();
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silence.assign(static_cast<size_t>(frames) * bytesPerFrame, 0);
                    samples = silence.data();
                }
                convert(samples, static_cast<int>(frames), static_cast<qint64>(capturedAt));
                reader->ReleaseBuffer(frames);
            }
        }

        void flush()
        {
            if (unsent.isEmpty())
                return;
            unsent.append(AUDIO_FRAME_SIZE - unsent.size(), '\0');
            emit owner->frameCaptured(unsent);
            unsent.clear();
        }

    private:
        void convert(const BYTE *samples, int frames, qint64 capturedAt)
        {
            const int room = swr_get_out_samples(resampler, frames);
            converted.resize(static_cast<size_t>(std::max(room, 0)) * AUDIO_CHANNELS);
            uint8_t *out = reinterpret_cast<uint8_t *>(converted.data());
            const uint8_t *in = samples;
            const int made = swr_convert(resampler, &out, room, &in, frames);
            if (made <= 0)
                return;

            // the resampler holds some frames back, so what comes out was captured that much earlier
            const qint64 heldBack = swr_get_delay(resampler, 10'000'000);
            remover.remove(converted.data(), made, capturedAt - heldBack);
            for (int i = 0; i < made * AUDIO_CHANNELS; i++) {
                const auto sample = static_cast<int16_t>(std::lround(std::clamp(converted[i], -1.0f, 32767.0f / 32768.0f) * 32768.0f));
                unsent.append(reinterpret_cast<const char *>(&sample), sizeof(sample));
            }
            while (unsent.size() >= AUDIO_FRAME_SIZE) {
                emit owner->frameCaptured(unsent.left(AUDIO_FRAME_SIZE));
                unsent.remove(0, AUDIO_FRAME_SIZE);
            }
        }

        LoopbackCapture *owner;
        SwrContext *resampler;
        int bytesPerFrame;
        PlayedSoundRemover remover;
        std::vector<BYTE> silence;
        std::vector<float> converted;
        QByteArray unsent;
    };

    // not every Windows with process loopback answers GetNextPacketSize for it
    HRESULT read(IAudioCaptureClient *reader, QByteArray &unsent)
    {
        for (;;) {
            BYTE *samples = nullptr;
            UINT32 sampleFrames = 0;
            DWORD flags = 0;
            const HRESULT result = reader->GetBuffer(&samples, &sampleFrames, &flags, nullptr, nullptr);
            if (FAILED(result))
                return result;
            if (result == AUDCLNT_S_BUFFER_EMPTY || sampleFrames == 0)
                return S_OK;

            const int size = static_cast<int>(sampleFrames) * BYTES_PER_SAMPLE_FRAME;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                unsent.append(size, '\0');
            else
                unsent.append(reinterpret_cast<const char *>(samples), size);
            reader->ReleaseBuffer(sampleFrames);

            while (unsent.size() >= AUDIO_FRAME_SIZE) {
                emit owner->frameCaptured(unsent.left(AUDIO_FRAME_SIZE));
                unsent.remove(0, AUDIO_FRAME_SIZE);
            }
        }
    }

    [[nodiscard]] bool superseded(quint64 request)
    {
        const std::lock_guard lock(mutex);
        return quitting || requests != request;
    }

    LoopbackCapture *owner;
    HANDLE asked = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE soundArrived = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    std::mutex mutex;
    std::optional<Target> wanted;
    quint64 requests = 0;
    bool quitting = false;
    std::thread thread;
};

bool processLoopbackAvailable()
{
    static const bool available = [] {
        bool activated = false;
        // its own thread, whatever COM model the caller's thread has
        std::thread([&activated] {
            const bool comStarted = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            {
                PendingActivation activation;
                HRESULT result = beginActivation({ GetCurrentProcessId(), LoopbackCapture::Scope::ProcessTree }, activation);
                if (SUCCEEDED(result) && WaitForSingleObject(activation.handler->completed(), ACTIVATION_TIMEOUT_MS) != WAIT_OBJECT_0)
                    result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                ComPointer<IAudioClient> client;
                if (SUCCEEDED(result))
                    result = activatedClient(activation, client);
                activated = SUCCEEDED(result);
                if (!activated)
                    qCInfo(LogVoice) << "This Windows cannot capture one program's sound:" << failureText("ActivateAudioInterfaceAsync", result);
            }
            if (comStarted)
                CoUninitialize();
        }).join();
        return activated;
    }();
    return available;
}

bool everythingButThisProgramCapturable()
{
    return true;
}

QList<PlayingProgram> playingPrograms()
{
    return {};
}

LoopbackCapture::LoopbackCapture(QObject *parent) : QObject(parent), worker(std::make_unique<Worker>(this)) {}

LoopbackCapture::~LoopbackCapture() = default;

void LoopbackCapture::start(quint32 processId, Scope scope)
{
    const bool ofEverythingButThis = scope == Scope::AllButProcessTree && processId == GetCurrentProcessId();
    const bool wholeDevice = ofEverythingButThis && (!processLoopbackAvailable() || QSettings().value(WHOLE_DEVICE_KEY, false).toBool());
    worker->request(Target{ processId, scope, wholeDevice });
}

void LoopbackCapture::stop()
{
    worker->request(std::nullopt);
}

} // namespace Audio
} // namespace Core
} // namespace Acheron

#endif
