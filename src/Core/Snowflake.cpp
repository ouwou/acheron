#include "Snowflake.hpp"

namespace Acheron {
namespace Core {

const Snowflake Snowflake::Invalid = -1ULL;

Snowflake::Snowflake() : value(Invalid) { }

Snowflake::Snowflake(quint64 n) : value(n) { }

bool Snowflake::isValid() const noexcept
{
    return *this != Invalid;
}

QString Snowflake::toString() const noexcept
{
    return QString::number(value);
}

Snowflake::operator quint64() const noexcept
{
    return value;
}

} // namespace Core
} // namespace Acheron