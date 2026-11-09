#pragma once

#include <QColor>
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
    QUrl imageUrl; // custom emoji or mention avatar
    QString mentionText; // `<@id>` / `<#id>` / `<@&id>` sent in place of insertText
    QString iconName; // Lucide icon drawn for a mention row without an avatar
    QColor iconColor; // invalid: the row's text colour

    [[nodiscard]] bool isCustom() const { return customId.isValid(); }
};

} // namespace Core
} // namespace Acheron
