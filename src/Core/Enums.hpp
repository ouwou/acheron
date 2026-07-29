#pragma once

#include <QMetaType>

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

Q_DECLARE_METATYPE(Acheron::Core::ConnectionState)