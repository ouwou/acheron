#pragma once

#include <QSize>
#include <QtGlobal>

#ifdef Q_OS_WIN

#include <map>
#include <memory>
#include <utility>

struct AVBufferRef;
struct AVFrame;
struct ID3D11Fence;
struct ID3D11Texture2D;
struct ID3D11VideoProcessor;
struct ID3D11VideoProcessorEnumerator;
struct ID3D11VideoProcessorInputView;
struct ID3D11VideoProcessorOutputView;

namespace Acheron {
namespace UI {

// every method needs the OpenGL context current
class D3D11GlBridge
{
public:
    [[nodiscard]] static bool contextSupportsIt();

    D3D11GlBridge();
    ~D3D11GlBridge();
    D3D11GlBridge(const D3D11GlBridge &) = delete;
    D3D11GlBridge &operator=(const D3D11GlBridge &) = delete;

    bool update(const AVFrame *frame, QSize size);

    [[nodiscard]] unsigned int texture() const { return glTexture; }
    bool lock();
    void unlock();

    // the destructor has no OpenGL context to do it in
    void release();

private:
    struct Wgl;

    bool openDevice(const AVFrame *frame);
    bool createTarget(int inputFormat, QSize input, QSize output);
    ID3D11VideoProcessorInputView *inputView(ID3D11Texture2D *texture, unsigned int slice);
    void releaseInputViews();
    void releaseTarget();

    std::unique_ptr<Wgl> wgl;
    AVBufferRef *hardwareDevice = nullptr;
    void *interopDevice = nullptr;
    // Windows 10 and later
    ID3D11Fence *fence = nullptr;
    void *fenceEvent = nullptr;
    quint64 fenceValue = 0;

    ID3D11Texture2D *target = nullptr;
    ID3D11VideoProcessorEnumerator *enumerator = nullptr;
    ID3D11VideoProcessor *processor = nullptr;
    ID3D11VideoProcessorOutputView *outputView = nullptr;
    std::map<std::pair<ID3D11Texture2D *, unsigned int>, ID3D11VideoProcessorInputView *> inputViews;
    const void *framePool = nullptr;
    void *interopObject = nullptr;
    unsigned int glTexture = 0;
    int targetInputFormat = 0;
    QSize inputSize;
    QSize outputSize;
    bool locked = false;
};

} // namespace UI
} // namespace Acheron

#endif // Q_OS_WIN
