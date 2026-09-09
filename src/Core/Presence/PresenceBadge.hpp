#pragma once

#include <QMetaType>

#include "Discord/Enums.hpp"

namespace Acheron {
namespace Core {

struct PresenceBadge
{
    Discord::StatusType status = Discord::StatusType::OFFLINE;
    bool mobile = false;
    bool streaming = false;
};

} // namespace Core
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Core::PresenceBadge)
