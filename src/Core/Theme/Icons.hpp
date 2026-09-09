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
inline constexpr auto ArrowDown = "arrow-down";
inline constexpr auto ArrowLeft = "arrow-left";
inline constexpr auto ArrowRight = "arrow-right";
inline constexpr auto AtSign = "at-sign";
inline constexpr auto Bell = "bell";
inline constexpr auto BookCheck = "book-check";
inline constexpr auto Bot = "bot";
inline constexpr auto ChartColumn = "chart-column";
inline constexpr auto ChevronRight = "chevron-right";
inline constexpr auto Compass = "compass";
inline constexpr auto Eye = "eye";
inline constexpr auto FileAudio = "file-audio";
inline constexpr auto FileText = "file-text";
inline constexpr auto Forward = "forward";
inline constexpr auto Gamepad = "gamepad-2";
inline constexpr auto Gem = "gem";
inline constexpr auto Hand = "hand";
inline constexpr auto Handshake = "handshake";
inline constexpr auto Hash = "hash";
inline constexpr auto IdCard = "id-card";
inline constexpr auto Image = "image";
inline constexpr auto Lock = "lock";
inline constexpr auto Maximize = "maximize";
inline constexpr auto Megaphone = "megaphone";
inline constexpr auto MessageCircle = "message-circle";
inline constexpr auto MessagesSquare = "messages-square";
inline constexpr auto Mic = "mic";
inline constexpr auto Minimize = "minimize";
inline constexpr auto Monitor = "monitor";
inline constexpr auto Music = "music";
inline constexpr auto Pause = "pause";
inline constexpr auto Pencil = "pencil";
inline constexpr auto Phone = "phone";
inline constexpr auto Pin = "pin";
inline constexpr auto Play = "play";
inline constexpr auto Radio = "radio";
inline constexpr auto Rss = "rss";
inline constexpr auto ShieldAlert = "shield-alert";
inline constexpr auto Spool = "spool";
inline constexpr auto TriangleAlert = "triangle-alert";
inline constexpr auto UserMinus = "user-minus";
inline constexpr auto UserPlus = "user-plus";
inline constexpr auto VolumeOff = "volume-x";
inline constexpr auto VolumeOn = "volume-2";
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
