#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <type_traits>

#include "Snowflake.hpp"

namespace Acheron {
namespace Core {
namespace JsonUtils {

namespace detail {
template <typename T>
struct is_qflags : std::false_type
{
};
template <typename T>
struct is_qflags<QFlags<T>> : std::true_type
{
};
} // namespace detail

template <typename T>
concept Serializable = requires(const T &t) {
    { t.toJson() } -> std::convertible_to<QJsonObject>;
};

template <typename T>
concept Deserializable = requires(const QJsonObject &object) {
    { T::fromJson(object) } -> std::convertible_to<T>;
};

template <typename T>
concept ListLike = requires(T t) {
    typename T::value_type;
    t.append(std::declval<typename T::value_type>());
    t.begin();
    t.end();
};

template <typename T>
concept MapLike = requires(T t) {
    typename T::key_type;
    typename T::mapped_type;
    t.begin();
    t.end();
    // qt
    t.begin().key();
    t.begin().value();
};

template <typename T>
concept QFlagType = detail::is_qflags<T>::value;

class JsonObject
{
protected:
    template <typename T, bool IsOptional = false, bool IsNullable = false>
    class Field
    {
    public:
        Field()
        {
            if constexpr (IsOptional) {
                state = State::Undefined;
            } else {
                state = State::Value;
                value = T();
            }
        }

        Field(const Field<T, IsOptional, IsNullable> &other)
            : state(other.state), value(other.value)
        {
        }

        Field(const T &value) : state(State::Value), value(value) { }

        Field(Field<T, IsOptional, IsNullable> &&other)
            : state(other.state), value(std::move(other.value))
        {
        }

        Field &operator=(const Field<T, IsOptional, IsNullable> &other)
        {
            state = other.state;
            value = other.value;
            return *this;
        }

        Field &operator=(Field<T, IsOptional, IsNullable> &&other)
        {
            state = other.state;
            value = std::move(other.value);
            return *this;
        }

        Field &operator=(const T &value)
        {
            this->value = value;
            state = State::Value;
            return *this;
        }

        Field &operator=(std::nullptr_t)
        {
            static_assert(IsNullable, "The field is not nullable");
            state = State::Null;
            return *this;
        }

        Field(std::nullptr_t)
        {
            static_assert(IsNullable, "The field is not nullable");
            state = State::Null;
        }

        template <typename U>
        bool operator==(const U &value) const
        {
            return this->value == value;
        }

        [[nodiscard]] bool isUndefined() const { return IsOptional && state == State::Undefined; }
        [[nodiscard]] bool isNull() const { return IsNullable && state == State::Null; }
        [[nodiscard]] bool hasValue() const { return state == State::Value; }

        T &get() { return value; }
        const T &get() const { return value; }
        T &operator*() { return value; }
        const T &operator*() const { return value; }
        T *operator->() { return &value; }
        const T *operator->() const { return &value; }
        operator T &() { return value; }
        operator const T &() const { return value; }
        operator QVariant() const
        {
            if constexpr (std::is_same_v<T, Snowflake>) {
                return static_cast<quint64>(value);
            } else {
                return value;
            }
        }

        void undefine()
        {
            static_assert(IsOptional, "The field is not optional");
            state = State::Undefined;
        }

    private:
        enum class State {
            Undefined,
            Null,
            Value,
        };
        State state;
        T value;
    };

    template <typename T>
    static QJsonValue toJsonValue(const T &value)
    {
        if constexpr (Serializable<T>) {
            return value.toJson();
        } else if constexpr (std::is_same_v<T, bool>) {
            return value;
        } else if constexpr (std::unsigned_integral<T>) {
            return static_cast<qint64>(value);
        } else if constexpr (std::is_enum_v<T> || QFlagType<T>) {
            return static_cast<qint64>(value);
        } else {
            return QJsonValue::fromVariant(QVariant::fromValue(value));
        }
    }

    template <typename T, bool IsOptional, bool IsNullable>
    static void insert(QJsonObject &object, const QString &key,
                       const Field<T, IsOptional, IsNullable> &field)
    {
        if (field.isUndefined())
            return;

        if (field.isNull()) {
            object[key] = QJsonValue::Null;
            return;
        }

        if constexpr (MapLike<T>) {
            QJsonObject mapObject;
            const auto &map = field.get();

            for (auto it = map.begin(); it != map.end(); ++it) {
                QString keyStr;

                if constexpr (std::is_same_v<typename T::key_type, Snowflake>) {
                    keyStr = QString::number(it.key());
                } else {
                    keyStr = it.key();
                }

                mapObject[keyStr] = toJsonValue(it.value());
            }
            object[key] = mapObject;
        } else if constexpr (std::is_same_v<T, QByteArray>) {
            QJsonArray array;
            for (unsigned char byte : field.get())
                array.append(static_cast<int>(byte));
            object[key] = array;
        } else if constexpr (!std::is_same_v<T, QString> && ListLike<T>) {
            QJsonArray array;
            for (const auto &element : field.get()) {
                array.append(toJsonValue(element));
            }
            object[key] = array;
        } else {
            object[key] = toJsonValue(field.get());
        }
    }

    template <typename T>
    static T fromJsonValue(const QJsonValue &value)
    {
        if constexpr (Deserializable<T>) {
            return T::fromJson(value.toObject());
        } else if constexpr (std::is_same_v<T, Snowflake>) {
            return value.toString().toULongLong();
        } else if constexpr (std::is_same_v<T, bool>) {
            return value.toBool();
        } else if constexpr (std::is_same_v<T, QString>) {
            return value.toString();
        } else if constexpr (QFlagType<T>) {
            return T::fromInt(value.toInt());
        } else if constexpr (std::is_enum_v<T>) {
            return static_cast<T>(value.toInt());
        } else if constexpr (std::is_same_v<T, QByteArray>) {
            QByteArray result;
            const QJsonArray array = value.toArray();
            result.reserve(array.size());
            for (const QJsonValue &element : array)
                result.append(static_cast<char>(element.toInt()));
            return result;
        } else if constexpr (ListLike<T>) {
            using InnerType = typename T::value_type;

            T list;
            const QJsonArray array = value.toArray();
            list.reserve(array.size());

            for (const QJsonValue &element : array) {
                list.append(fromJsonValue<InnerType>(element));
            }
            return list;
        } else {
            return value.toVariant().value<T>();
        }
    }

    template <typename T, bool IsOptional, bool IsNullable>
    static void get(const QJsonObject &object, const QString &key,
                    Field<T, IsOptional, IsNullable> &field)
    {
        auto it = object.find(key);

        if (it == object.end()) {
            if constexpr (IsOptional) {
                field.undefine();
            }
            return;
        }

        if (it.value().isNull()) {
            if constexpr (IsNullable) {
                field = nullptr;
            }
            return;
        }

        field = fromJsonValue<T>(it.value());
    }
};

} // namespace JsonUtils
} // namespace Core
} // namespace Acheron
