#pragma once

#include <QString>
#include <optional>

namespace Acheron {
namespace Core {

template <typename T>
struct Result
{
    std::optional<T> value;
    QString error;

    bool success() const { return value.has_value(); }

    static Result<T> makeOk(const T &value)
    {
        Result<T> result{ value, "" };
        return result;
    }

    static Result<T> makeError(const QString &error)
    {
        Result<T> result{ {}, error };
        return result;
    }
};

} // namespace Core
} // namespace Acheron
