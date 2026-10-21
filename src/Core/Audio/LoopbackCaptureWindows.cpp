#include "Core/Audio/LoopbackCapture.hpp"

#ifdef Q_OS_WIN

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Logging.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>

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

            const QString failure = capture(*target, request);
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

QList<PlayingProgram> playingPrograms()
{
    return {};
}

LoopbackCapture::LoopbackCapture(QObject *parent) : QObject(parent), worker(std::make_unique<Worker>(this)) {}

LoopbackCapture::~LoopbackCapture() = default;

void LoopbackCapture::start(quint32 processId, Scope scope)
{
    worker->request(Target{ processId, scope });
}

void LoopbackCapture::stop()
{
    worker->request(std::nullopt);
}

} // namespace Audio
} // namespace Core
} // namespace Acheron

#endif
