#pragma once

#include <QString>

#include "Snowflake.hpp"

namespace Acheron {
namespace Core {

class TokenStore
{
public:
    static constexpr char const *SERVICE_NAME = "Acheron";

    static bool saveToken(Snowflake accountId, const QString &token);
    static QString loadToken(Snowflake accountId);
    static bool deleteToken(Snowflake accountId);

private:
    static QString keyForAccount(Snowflake accountId);
};

} // namespace Core
} // namespace Acheron
