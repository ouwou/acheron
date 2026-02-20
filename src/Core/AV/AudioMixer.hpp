#pragma once

#include <QByteArray>
#include <QVector>

#include <cstdint>
#include <utility>

namespace Acheron {
namespace Core {
namespace AV {

class AudioMixer
{
public:
    // all frames must be same size
    static QByteArray mix(const QVector<std::pair<QByteArray /* pcm */, float /* gain */>> &inputs);
};

} // namespace AV
} // namespace Core
} // namespace Acheron
