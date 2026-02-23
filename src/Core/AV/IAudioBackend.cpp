#include "IAudioBackend.hpp"
#include "MiniaudioAudioBackend.hpp"

namespace Acheron {
namespace Core {
namespace AV {

std::unique_ptr<IAudioBackend> IAudioBackend::create(QObject *parent)
{
    return std::make_unique<MiniaudioAudioBackend>(parent);
}

} // namespace AV
} // namespace Core
} // namespace Acheron
