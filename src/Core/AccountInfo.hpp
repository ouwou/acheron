#pragma once

#include <QString>
#include <QPixmap>
#include <QMetaType>

#include "Enums.hpp"
#include "Snowflake.hpp"

namespace Acheron {
namespace Core {
struct AccountInfo
{
    Snowflake id;
    QString username;
    QString displayName;
    QString token;
    QPixmap avatar;
    ConnectionState state = ConnectionState::Disconnected;
};
} // namespace Core
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Core::AccountInfo)
