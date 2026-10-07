#include "UI/Emoji/EmojiGlyphs.hpp"

#include <QCache>
#include <QCoreApplication>
#include <QGlyphRun>
#include <QPainter>
#include <QRawFont>
#include <QTextLayout>

#include <algorithm>

#include "Core/Theme/Manager.hpp"

namespace Acheron {
namespace UI {
namespace EmojiGlyphs {

namespace {

constexpr int CacheBudgetKiB = 8 * 1024;

struct GlyphKey
{
    QString surrogates;
    int physicalPx;

    bool operator==(const GlyphKey &other) const
    {
        return physicalPx == other.physicalPx && surrogates == other.surrogates;
    }
};

size_t qHash(const GlyphKey &key, size_t seed = 0)
{
    return qHashMulti(seed, key.surrogates, key.physicalPx);
}

QCache<GlyphKey, QPixmap> &glyphCache()
{
    static QCache<GlyphKey, QPixmap> cache(CacheBudgetKiB);
    static const bool clearsOnThemeChange = [] {
        const auto clear = [] { cache.clear(); };
        QObject::connect(&Core::Theme::Manager::instance(), &Core::Theme::Manager::themeChanged, qApp, clear);
        QObject::connect(&Core::Theme::Manager::instance(), &Core::Theme::Manager::metricsChanged, qApp, clear);
        return true;
    }();
    Q_UNUSED(clearsOnThemeChange);
    return cache;
}

struct PlacedGlyph
{
    QImage image;
    qreal deviceLeft = 0;
};

QImage tinted(const QImage &coverage, const QColor &color)
{
    QImage image(coverage.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.drawImage(0, 0, coverage.convertToFormat(QImage::Format_Alpha8));
    return image;
}

QList<PlacedGlyph> rasterise(const QString &surrogates, const QFont &font, qreal devicePixelRatio, const QColor &textColor)
{
    QTextLayout layout(surrogates, font);
    layout.beginLayout();
    layout.createLine();
    layout.endLayout();

    const QTransform toDevice = QTransform::fromScale(devicePixelRatio, devicePixelRatio);
    QList<PlacedGlyph> glyphs;
    for (const QGlyphRun &run : layout.glyphRuns()) {
        const QRawFont rawFont = run.rawFont();
        const auto indexes = run.glyphIndexes();
        const auto positions = run.positions();
        for (qsizetype i = 0; i < indexes.size(); i++) {
            const QImage image = rawFont.alphaMapForGlyph(indexes.at(i), QRawFont::PixelAntialiasing, toDevice);
            if (image.isNull())
                continue;
            const bool isColourGlyph = image.depth() == 32;
            glyphs.append({ isColourGlyph ? image : tinted(image, textColor), positions.at(i).x() * devicePixelRatio });
        }
    }
    return glyphs;
}

QPixmap render(const QString &surrogates, int px, qreal devicePixelRatio)
{
    const Core::Theme::Manager &theme = Core::Theme::Manager::instance();
    const int physicalPx = qRound(px * devicePixelRatio);

    QPixmap pixmap(physicalPx, physicalPx);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);

    QFont font = theme.font(Core::Theme::FontRole::Message);
    font.setPixelSize(px * 4 / 5);
    const QList<PlacedGlyph> glyphs = rasterise(surrogates, font, devicePixelRatio, theme.color(Core::Theme::Token::PrimaryText));
    if (glyphs.isEmpty())
        return pixmap;

    qreal left = glyphs.first().deviceLeft;
    qreal right = left;
    int height = 0;
    for (const PlacedGlyph &glyph : glyphs) {
        left = std::min(left, glyph.deviceLeft);
        right = std::max(right, glyph.deviceLeft + glyph.image.width());
        height = std::max(height, glyph.image.height());
    }
    const qreal scale = std::min({ qreal(1), physicalPx / (right - left), physicalPx / qreal(height) });

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.scale(1 / devicePixelRatio, 1 / devicePixelRatio);
    painter.translate((physicalPx - (right - left) * scale) / 2, physicalPx / 2.0);
    painter.scale(scale, scale);
    for (const PlacedGlyph &glyph : glyphs)
        painter.drawImage(QPointF(glyph.deviceLeft - left, -glyph.image.height() / 2.0), glyph.image);
    return pixmap;
}

} // namespace

QPixmap pixmap(const QString &surrogates, int px, qreal devicePixelRatio)
{
    const GlyphKey key{ surrogates, qRound(px * devicePixelRatio) };
    QCache<GlyphKey, QPixmap> &cache = glyphCache();
    if (const QPixmap *cached = cache.object(key))
        return *cached;

    const QPixmap rendered = render(surrogates, px, devicePixelRatio);
    const int costKiB = std::max(1, key.physicalPx * key.physicalPx * 4 / 1024);
    cache.insert(key, new QPixmap(rendered), costKiB);
    return rendered;
}

} // namespace EmojiGlyphs
} // namespace UI
} // namespace Acheron
