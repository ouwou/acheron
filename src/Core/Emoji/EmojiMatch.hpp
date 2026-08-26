#pragma once

#include <QString>
#include <QUrl>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace Core {

struct EmojiMatch
{
    // replacement. `:name:` or `<:name:id>` / `<a:name:id>`
    QString insertText;
    QString displayLabel;
    QString surrogates; // unicode only
    Snowflake customId; // custom only
    QUrl imageUrl; // custom only

    [[nodiscard]] bool isCustom() const { return customId.isValid(); }
};

} // namespace Core
} // namespace Acheron
