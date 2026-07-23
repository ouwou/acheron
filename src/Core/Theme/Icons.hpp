#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

#include "Core/Theme/Tokens.hpp"

namespace Acheron {
namespace Core {
namespace Theme {
namespace Icons {

namespace Name {
inline constexpr auto AtSign = "at-sign";
inline constexpr auto Eye = "eye";
inline constexpr auto FileText = "file-text";
inline constexpr auto Handshake = "handshake";
inline constexpr auto IdCard = "id-card";
inline constexpr auto Lock = "lock";
inline constexpr auto MessageCircle = "message-circle";
inline constexpr auto Pencil = "pencil";
inline constexpr auto Play = "play";
inline constexpr auto Spool = "spool";
inline constexpr auto X = "x";
} // namespace Name

QPixmap pixmap(const QString &name, int px, const QColor &color, qreal dpr = 1.0);
QPixmap pixmap(const QString &name, int px, Token token, qreal dpr = 1.0);

QIcon icon(const QString &name, const QColor &color);
QIcon icon(const QString &name, Token token);

} // namespace Icons
} // namespace Theme
} // namespace Core
} // namespace Acheron
