#pragma once

namespace Acheron {
namespace Core {

enum class ConnectionState {
    Disconnecting,
    Disconnected,
    Connecting,
    Connected,
};

} // namespace Core
} // namespace Acheron