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

    int sampleCount = frameSize / static_cast<int>(sizeof(int16_t));

    QVector<int32_t> mixed(sampleCount, 0);

    for (const auto &[pcm, gain] : inputs) {
        if (pcm.size() != frameSize)
            continue;

        const auto *samples = reinterpret_cast<const int16_t *>(pcm.constData());
        for (int i = 0; i < sampleCount; i++)
            mixed[i] += static_cast<int32_t>(std::lround(samples[i] * gain));
    }

    QByteArray output(frameSize, '\0');
    auto *out = reinterpret_cast<int16_t *>(output.data());
    for (int i = 0; i < sampleCount; i++)
        out[i] = static_cast<int16_t>(std::clamp(mixed[i],
                                                 static_cast<int32_t>(INT16_MIN),
                                                 static_cast<int32_t>(INT16_MAX)));

    return output;
}

} // namespace AV
} // namespace Core
} // namespace Acheron
