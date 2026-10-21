#pragma once

#include <QString>
#include <QUrl>

#include <optional>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Discord {

// `guild:<guild>:<channel>:<owner>` or `call:<channel>:<owner>`
struct StreamKey
{
    enum class Type {
        Guild,
        Call,
    };

    Type type = Type::Guild;
    Core::Snowflake guildId;
    Core::Snowflake channelId;
    Core::Snowflake ownerId;

    [[nodiscard]] static StreamKey forChannel(Core::Snowflake guildId, Core::Snowflake channelId, Core::Snowflake ownerId);
    [[nodiscard]] static std::optional<StreamKey> parse(const QString &key);

    [[nodiscard]] QString toString() const;
    [[nodiscard]] QString typeName() const;

    bool operator==(const StreamKey &other) const = default;
};

struct StreamPreviewLookup
{
    QUrl url;
    int statusCode = 0;
    qint64 retryAfterMs = 0;
};

} // namespace Discord
} // namespace Acheron
