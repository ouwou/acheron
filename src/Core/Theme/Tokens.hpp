#pragma once

#include <QColor>
#include <QHashFunctions>
#include <QPalette>
#include <QString>

#include <optional>
#include <type_traits>
#include <vector>

namespace Acheron {
namespace Core {
namespace Theme {

enum class Token {
    // backgrounds
    WindowBg,
    BaseBg,
    AlternateBaseBg,
    ButtonBg,
    TooltipBg,
    // text
    WindowText,
    PrimaryText,
    ButtonText,
    TooltipText,
    PlaceholderText,
    DisabledText,
    BrightText,
    // accents
    Highlight,
    HighlightedText,
    LinkText,
    Divider,
    // extended chat elements
    ChatError,
    EmbedDefault,
    MentionText,
    MentionBg,
    // presence
    StatusOnline,
    StatusIdle,
    StatusDnd,
    StatusOffline,
    StatusStreaming,
};

struct TokenDescriptor
{
    Token token;
    const char *id;
    const char *label;
    const char *group;
    QColor defaultColor;
    std::optional<QPalette::ColorRole> role; // nullopt = extended
    bool supportsAlpha = false;
};

const std::vector<TokenDescriptor> &registry();
const TokenDescriptor *findById(const QString &id);
const TokenDescriptor &descriptor(Token token);

inline size_t qHash(Token key, size_t seed = 0) noexcept
{
    return ::qHash(static_cast<std::underlying_type_t<Token>>(key), seed);
}

} // namespace Theme
} // namespace Core
} // namespace Acheron
