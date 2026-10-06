#pragma once

#include <QCoreApplication>
#include <QString>

#include "Result.hpp"
#include "Snowflake.hpp"

namespace Acheron {
namespace Core {

class TokenStore
{
    Q_DECLARE_TR_FUNCTIONS(Acheron::Core::TokenStore)

public:
    static constexpr char const *SERVICE_NAME = "Acheron";

    static Result<void> saveToken(Snowflake accountId, const QString &token);
    static Result<QString> loadToken(Snowflake accountId);
    static bool deleteToken(Snowflake accountId);

private:
    static QString keyForAccount(Snowflake accountId);
    static QString describeKeychainFailure(const QString &keychainError);
};

} // namespace Core
} // namespace Acheron
