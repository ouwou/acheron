#include "NoiseSuppressor.hpp"
#include "IAudioBackend.hpp"

#ifdef ACHERON_HAVE_RNNOISE
#include <rnnoise.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Acheron {
namespace Core {
namespace AV {

#ifdef ACHERON_HAVE_RNNOISE
static int16_t floatToSample(float v)
{
    v = std::clamp(v, -32768.0f, 32767.0f);
    return static_cast<int16_t>(std::lrintf(v));
}
#endif

NoiseSuppressor::NoiseSuppressor() = default;

NoiseSuppressor::~NoiseSuppressor()
{
#ifdef ACHERON_HAVE_RNNOISE
    for (DenoiseState *s : states) {
        if (s)
            rnnoise_destroy(s);
    }
#endif
}

bool NoiseSuppressor::init()
{
#ifdef ACHERON_HAVE_RNNOISE
    states[0] = rnnoise_create(nullptr);
    return states[0] != nullptr;
#else
    return false;
#endif
}

void NoiseSuppressor::reconfigure(int channels)
{
#ifdef ACHERON_HAVE_RNNOISE
    const bool wantStereo = channels >= 2;
    const bool haveStereo = states[1] != nullptr;
    if (wantStereo == haveStereo)
        return;

    if (wantStereo) {
        states[1] = rnnoise_create(nullptr);
    } else {
        rnnoise_destroy(states[1]);
        states[1] = nullptr;
    }
#else
    (void)channels;
#endif
}

QByteArray NoiseSuppressor::process(const QByteArray &pcmFrame, float &outVoiceProb)
{
    outVoiceProb = -1.0f;

#ifdef ACHERON_HAVE_RNNOISE
    if (!states[0] || pcmFrame.size() != AUDIO_FRAME_SIZE)
        return pcmFrame;

    const int frameSize = rnnoise_get_frame_size();
    const auto *in = reinterpret_cast<const int16_t *>(pcmFrame.constData());

    QByteArray out(AUDIO_FRAME_SIZE, '\0');
    auto *outSamples = reinterpret_cast<int16_t *>(out.data());
    float prob = 0.0f;

    if (states[1]) {
        // stereo
        std::array<float, AUDIO_FRAME_SAMPLES> left;
        std::array<float, AUDIO_FRAME_SAMPLES> right;
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            left[i] = static_cast<float>(in[i * AUDIO_CHANNELS]);
            right[i] = static_cast<float>(in[i * AUDIO_CHANNELS + 1]);
        }
        for (int off = 0; off + frameSize <= AUDIO_FRAME_SAMPLES; off += frameSize) {
            float pl = rnnoise_process_frame(states[0], left.data() + off, left.data() + off);
            float pr = rnnoise_process_frame(states[1], right.data() + off, right.data() + off);
            prob = std::max({ prob, pl, pr });
        }
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            outSamples[i * AUDIO_CHANNELS] = floatToSample(left[i]);
            outSamples[i * AUDIO_CHANNELS + 1] = floatToSample(right[i]);
        }
    } else {
        // mono
        std::array<float, AUDIO_FRAME_SAMPLES> mono;
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++)
            mono[i] = static_cast<float>(in[i * AUDIO_CHANNELS]);
        for (int off = 0; off + frameSize <= AUDIO_FRAME_SAMPLES; off += frameSize) {
            float p = rnnoise_process_frame(states[0], mono.data() + off, mono.data() + off);
            prob = std::max(prob, p);
        }
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            int16_t s = floatToSample(mono[i]);
            outSamples[i * AUDIO_CHANNELS] = s;
            outSamples[i * AUDIO_CHANNELS + 1] = s;
        }
    }

    outVoiceProb = prob;
    return out;
#else
    return pcmFrame;
#endif
}

} // namespace AV
} // namespace Core
} // namespace Acheron
