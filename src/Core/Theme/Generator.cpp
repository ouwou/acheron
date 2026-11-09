#include "Core/Theme/Generator.hpp"

#include "Core/Theme/Tokens.hpp"

#include <algorithm>
#include <cmath>

namespace Acheron {
namespace Core {
namespace Theme {

namespace {

QColor hsl(qreal hueDeg, qreal s, qreal l)
{
    qreal h = std::fmod(hueDeg, 360.0);
    if (h < 0.0)
        h += 360.0;
    s = std::clamp(s, 0.0, 1.0);
    l = std::clamp(l, 0.0, 1.0);
    return QColor::fromHslF(static_cast<float>(h / 360.0), static_cast<float>(s), static_cast<float>(l));
}

struct AccentHues
{
    qreal primary;
    qreal secondary;
};

AccentHues accentHuesFor(Scheme scheme, qreal seedHue)
{
    switch (scheme) {
    case Scheme::Monochromatic:
        return { seedHue, seedHue };
    case Scheme::Analogous:
        return { seedHue + 30.0, seedHue - 30.0 };
    case Scheme::Complementary:
        return { seedHue + 180.0, seedHue + 180.0 };
    case Scheme::SplitComplementary:
        return { seedHue + 150.0, seedHue + 210.0 };
    case Scheme::Triadic:
        return { seedHue + 120.0, seedHue + 240.0 };
    }
    return { seedHue, seedHue };
}

} // namespace

QString schemeName(Scheme scheme)
{
    switch (scheme) {
    case Scheme::Monochromatic:
        return "Monochromatic";
    case Scheme::Analogous:
        return "Analogous";
    case Scheme::Complementary:
        return "Complementary";
    case Scheme::SplitComplementary:
        return "Split-complementary";
    case Scheme::Triadic:
        return "Triadic";
    }
    return QString();
}

QHash<Token, QColor> generate(const QColor &seed, Scheme scheme, bool dark)
{
    const qreal rawHue = seed.hslHueF();
    const qreal seedHue = (rawHue < 0.0) ? 250.0 : rawHue * 360.0;
    const qreal seedSat = seed.hslSaturationF();
    const AccentHues accents = accentHuesFor(scheme, seedHue);
    const qreal accentHue = accents.primary;
    const qreal linkHue = accents.secondary;

    const qreal bgSat = std::clamp(seedSat * 0.5, 0.10, 0.45);
    const qreal textSat = std::clamp(seedSat * 0.35, 0.04, 0.22);
    const qreal accSat = std::clamp(std::max(seedSat, 0.55), 0.5, 0.95);

    QHash<Token, QColor> m;

    if (dark) {
        m.insert(Token::WindowBg, hsl(seedHue, bgSat, 0.085));
        m.insert(Token::BaseBg, hsl(seedHue, bgSat, 0.12));
        m.insert(Token::AlternateBaseBg, hsl(seedHue, bgSat, 0.16));
        m.insert(Token::ButtonBg, hsl(seedHue, bgSat, 0.19));
        m.insert(Token::TooltipBg, hsl(seedHue, bgSat, 0.19));

        m.insert(Token::WindowText, hsl(seedHue, textSat, 0.88));
        m.insert(Token::PrimaryText, hsl(seedHue, textSat, 0.84));
        m.insert(Token::ButtonText, hsl(seedHue, textSat, 0.88));
        m.insert(Token::TooltipText, hsl(seedHue, textSat, 0.92));
        m.insert(Token::PlaceholderText, hsl(seedHue, textSat, 0.55));
        m.insert(Token::DisabledText, hsl(seedHue, textSat, 0.45));
        m.insert(Token::BrightText, hsl(accentHue, accSat, 0.75));

        m.insert(Token::Highlight, hsl(accentHue, accSat, 0.60));
        m.insert(Token::HighlightedText, hsl(accentHue, 0.25, 0.97));
        m.insert(Token::LinkText, hsl(linkHue, 0.70, 0.72));
        m.insert(Token::Divider, hsl(seedHue, bgSat, 0.30));

        m.insert(Token::MentionText, hsl(accentHue, 0.65, 0.82));
        QColor mentionBg = hsl(accentHue, accSat, 0.58);
        mentionBg.setAlpha(77); // ~30%
        m.insert(Token::MentionBg, mentionBg);
    } else {
        m.insert(Token::WindowBg, hsl(seedHue, bgSat, 0.97));
        m.insert(Token::BaseBg, hsl(seedHue, bgSat, 0.99));
        m.insert(Token::AlternateBaseBg, hsl(seedHue, bgSat, 0.93));
        m.insert(Token::ButtonBg, hsl(seedHue, bgSat, 0.90));
        m.insert(Token::TooltipBg, hsl(seedHue, bgSat, 0.92));

        m.insert(Token::WindowText, hsl(seedHue, textSat, 0.18));
        m.insert(Token::PrimaryText, hsl(seedHue, textSat, 0.22));
        m.insert(Token::ButtonText, hsl(seedHue, textSat, 0.18));
        m.insert(Token::TooltipText, hsl(seedHue, textSat, 0.16));
        m.insert(Token::PlaceholderText, hsl(seedHue, textSat, 0.50));
        m.insert(Token::DisabledText, hsl(seedHue, textSat, 0.60));
        m.insert(Token::BrightText, hsl(accentHue, accSat, 0.40));

        m.insert(Token::Highlight, hsl(accentHue, accSat, 0.55));
        m.insert(Token::HighlightedText, hsl(accentHue, 0.20, 0.99));
        m.insert(Token::LinkText, hsl(linkHue, 0.75, 0.45));
        m.insert(Token::Divider, hsl(seedHue, bgSat, 0.78));

        m.insert(Token::MentionText, hsl(accentHue, 0.70, 0.40));
        QColor mentionBg = hsl(accentHue, accSat, 0.55);
        mentionBg.setAlpha(64); // ~25%
        m.insert(Token::MentionBg, mentionBg);
    }

    // the official mention highlight is the same warning amber on every theme; only its bar darkens on light ones
    m.insert(Token::MentionHighlightBg, descriptor(Token::MentionHighlightBg).defaultColor);
    m.insert(Token::MentionHighlightBar, dark ? descriptor(Token::MentionHighlightBar).defaultColor : QColor(148, 84, 0));
    m.insert(Token::ChatError, hsl(2.0, 0.70, dark ? 0.62 : 0.50));
    m.insert(Token::EmbedDefault, m.value(Token::Highlight));

    return m;
}

} // namespace Theme
} // namespace Core
} // namespace Acheron
