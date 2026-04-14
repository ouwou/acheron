#include "AudioMixer.hpp"
#include <algorithm>
#include <cmath>
namespace Acheron {
namespace Core {
namespace AV {
QByteArray AudioMixer::mix(const QVector<std::pair<QByteArray, float>> &inputs)
{
    if (inputs.isEmpty())
        return {};
    int frameSize = inputs.first().first.size();
    if (frameSize == 0)
        return {};
    int sampleCount = frameSize / static_cast<int>(sizeof(float));
    QVector<float> mixed(sampleCount, 0.0f);
    for (const auto &[pcm, gain] : inputs) {
        if (pcm.size() != frameSize)
            continue;
        const auto *samples = reinterpret_cast<const float *>(pcm.constData());
        for (int i = 0; i < sampleCount; i++)
            mixed[i] += samples[i] * gain;
    }
    QByteArray output(frameSize, '\0');
    auto *out = reinterpret_cast<float *>(output.data());
    for (int i = 0; i < sampleCount; i++)
        out[i] = std::clamp(mixed[i], -1.0f, 1.0f);
    return output;
}
} // namespace AV
} // namespace Core
} // namespace Acheron
