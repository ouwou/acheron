#pragma once

#include <QCoreApplication>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>

#include "Core/Theme/Manager.hpp"

namespace Acheron {
namespace UI {

inline constexpr qreal LIVE_PILL_FONT_SCALE = 0.75;
inline constexpr int LIVE_PILL_PADDING = 4;
inline constexpr int LIVE_PILL_RADIUS = 3;

inline QString livePillText()
{
    return QCoreApplication::translate("Acheron::UI::LivePill", "LIVE");
}

inline QFont livePillFont(QFont font)
{
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * LIVE_PILL_FONT_SCALE);
    return font;
}

inline QColor livePillColour()
{
    return Core::Theme::Manager::instance().color(Core::Theme::Token::ChatError);
}

inline int livePillWidth(const QFont &base)
{
    return QFontMetrics(livePillFont(base)).horizontalAdvance(livePillText()) + 2 * LIVE_PILL_PADDING;
}

inline void paintLivePill(QPainter *painter, const QRect &pill)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(livePillColour());
    painter->drawRoundedRect(pill, LIVE_PILL_RADIUS, LIVE_PILL_RADIUS);
    painter->setFont(livePillFont(painter->font()));
    painter->setPen(Qt::white);
    painter->drawText(pill, Qt::AlignCenter, livePillText());
    painter->restore();
}

} // namespace UI
} // namespace Acheron
