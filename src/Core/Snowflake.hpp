#pragma once

#include <QMetaType>

namespace Acheron {
namespace Core {

class Snowflake
{
public:
    Snowflake();
    Snowflake(quint64 n);

    [[nodiscard]] bool isValid() const noexcept;

    auto operator<=>(const Snowflake &other) const noexcept { return value <=> other.value; }

    QString toString() const noexcept;
    // operator QString() const noexcept;

    operator quint64() const noexcept;

    const static Snowflake Invalid;
    const static quint64 SecondsInterval = 4194304000ULL;

private:
    friend struct ::std::hash<Snowflake>;
    friend struct ::std::less<Snowflake>;

    quint64 value;
};

} // namespace Core
} // namespace Acheron

namespace std {
template <>
struct hash<Acheron::Core::Snowflake>
{
    std::size_t operator()(const Acheron::Core::Snowflake &k) const { return k.value; }
};

template <>
struct less<Acheron::Core::Snowflake>
{
    bool operator()(const Acheron::Core::Snowflake &l, const Acheron::Core::Snowflake &r) const
    {
        return l.value < r.value;
    }
};
} // namespace std

Q_DECLARE_METATYPE(Acheron::Core::Snowflake)
