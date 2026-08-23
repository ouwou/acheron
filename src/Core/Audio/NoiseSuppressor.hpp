#pragma once

#include <QByteArray>

struct DenoiseState;

namespace Acheron {
namespace Core {
namespace Audio {

class NoiseSuppressor
{
public:
    NoiseSuppressor();
    ~NoiseSuppressor();

    NoiseSuppressor(const NoiseSuppressor &) = delete;
    NoiseSuppressor &operator=(const NoiseSuppressor &) = delete;

    bool init();

    void reconfigure(int channels);

    // 960 samples stereo int16
    QByteArray process(const QByteArray &pcmFrame, float &outVoiceProb);

private:
    DenoiseState *states[2] = { nullptr, nullptr };
};

} // namespace Audio
} // namespace Core
} // namespace Acheron
