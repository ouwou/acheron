#include "IAudioBackend.hpp"
#include "MiniaudioAudioBackend.hpp"

namespace Acheron {
namespace Core {
namespace Audio {

std::unique_ptr<IAudioBackend> IAudioBackend::create(QObject *parent)
{
    return std::make_unique<MiniaudioAudioBackend>(parent);
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
