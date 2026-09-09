#include "Tokens.hpp"

namespace Acheron {
namespace Core {
namespace Theme {

const std::vector<TokenDescriptor> &registry()
{
    static const std::vector<TokenDescriptor> reg = {
        // backgrounds
        { Token::WindowBg, "window.bg", "Window background", "Backgrounds", QColor(24, 22, 34),
          QPalette::Window, false },
        { Token::BaseBg, "base.bg", "Base", "Backgrounds", QColor(30, 28, 44),
          QPalette::Base, false },
        { Token::AlternateBaseBg, "base.alt", "Alternate base", "Backgrounds", QColor(36, 34, 54),
          QPalette::AlternateBase, false },
        { Token::ButtonBg, "button.bg", "Button", "Backgrounds", QColor(40, 38, 60),
          QPalette::Button, false },
        { Token::TooltipBg, "tooltip.bg", "Tooltip background", "Backgrounds", QColor(40, 38, 60),
          QPalette::ToolTipBase, false },

        // text
        { Token::WindowText, "text.window", "Window text", "Text", QColor(210, 208, 225),
          QPalette::WindowText, false },
        { Token::PrimaryText, "text.primary", "Primary text", "Text", QColor(200, 198, 220),
          QPalette::Text, false },
        { Token::ButtonText, "text.button", "Button text", "Text", QColor(210, 208, 225),
          QPalette::ButtonText, false },
        { Token::TooltipText, "text.tooltip", "Tooltip text", "Text", QColor(220, 218, 240),
          QPalette::ToolTipText, false },
        { Token::PlaceholderText, "text.placeholder", "Placeholder text", "Text",
          QColor(126, 124, 153), QPalette::PlaceholderText, false },
        { Token::DisabledText, "text.disabled", "Disabled text", "Text", QColor(126, 124, 153),
          std::nullopt, false },
        { Token::BrightText, "text.bright", "Bright text", "Text", QColor(189, 146, 236),
          QPalette::BrightText, false },

        // accents
        { Token::Highlight, "accent.highlight", "Highlight", "Accents",
          QColor(124, 92, 192), QPalette::Highlight, false },
        { Token::HighlightedText, "accent.highlightText", "Highlighted text", "Accents",
          QColor(245, 244, 255), QPalette::HighlightedText, false },
        { Token::LinkText, "accent.link", "Links", "Accents", QColor(157, 180, 255), QPalette::Link,
          false },
        { Token::Divider, "accent.divider", "Dividers", "Accents", QColor(58, 55, 88),
          QPalette::Mid, false },

        // extended chat elements
        { Token::ChatError, "chat.error", "Failed message", "Chat", QColor(220, 50, 50),
          std::nullopt, false },
        { Token::EmbedDefault, "chat.embedDefault", "Default embed color", "Chat",
          QColor(88, 101, 242), std::nullopt, true },
        { Token::MentionText, "chat.mentionText", "Mention text", "Chat", QColor(201, 205, 251),
          std::nullopt, false },
        { Token::MentionBg, "chat.mentionBg", "Mention background", "Chat",
          QColor(88, 101, 242, 77), std::nullopt, true },

        // presence
        { Token::StatusOnline, "status.online", "Online", "Presence", QColor(35, 165, 90),
          std::nullopt, false },
        { Token::StatusIdle, "status.idle", "Idle", "Presence", QColor(240, 178, 50),
          std::nullopt, false },
        { Token::StatusDnd, "status.dnd", "Do not disturb", "Presence", QColor(242, 63, 67),
          std::nullopt, false },
        { Token::StatusOffline, "status.offline", "Offline", "Presence", QColor(128, 132, 142),
          std::nullopt, false },
        { Token::StatusStreaming, "status.streaming", "Streaming", "Presence", QColor(89, 54, 149),
          std::nullopt, false },
    };
    return reg;
}

const TokenDescriptor *findById(const QString &id)
{
    for (const TokenDescriptor &d : registry()) {
        if (id == QLatin1String(d.id))
            return &d;
    }
    return nullptr;
}

const TokenDescriptor &descriptor(Token token)
{
    for (const TokenDescriptor &d : registry()) {
        if (d.token == token)
            return d;
    }
    return registry().front(); // just in case
}

} // namespace Theme
} // namespace Core
} // namespace Acheron
