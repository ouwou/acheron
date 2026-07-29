#include "Core/Theme/Icons.hpp"

#include "Core/Logging.hpp"
#include "Core/Theme/Manager.hpp"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QSvgRenderer>

#include <optional>
#include <utility>

namespace Acheron {
namespace Core {
namespace Theme {
namespace Icons {

namespace {

QByteArray svgSource(const QString &name)
{
    static QHash<QString, QByteArray> cache;
    auto it = cache.constFind(name);
    if (it != cache.constEnd())
        return it.value();

    QByteArray data;
    QFile file(QStringLiteral(":/resources/icons/lucide/%1.svg").arg(name));
    if (file.open(QIODevice::ReadOnly))
        data = file.readAll();
    else
        qCWarning(LogUI) << "Theme::Icons: missing icon resource" << name;

    cache.insert(name, data);
    return data;
}

QPixmap render(const QString &name, int px, const QColor &color, qreal dpr)
{
    QByteArray svg = svgSource(name);
    if (svg.isEmpty())
        return {};

    svg.replace(QByteArrayLiteral("currentColor"), color.name(QColor::HexRgb).toUtf8());

    const int side = qMax(1, qRound(px * dpr));
    QPixmap pm(side, side);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QSvgRenderer renderer(svg);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&p, QRectF(0, 0, px, px));
    p.end();
    return pm;
}

class LucideIconEngine : public QIconEngine
{
public:
    LucideIconEngine(QString name, QColor color)
        : iconName(std::move(name)), fixedColor(color)
    {
    }
    LucideIconEngine(QString name, Token token)
        : iconName(std::move(name)), themeToken(token)
    {
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        Q_UNUSED(state);
        const QPaintDevice *dev = painter->device();
        const qreal dpr = dev ? dev->devicePixelRatioF() : 1.0;
        painter->drawPixmap(rect, renderFor(rect.size(), mode, dpr));
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        Q_UNUSED(state);
        return renderFor(size, mode, 1.0);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        Q_UNUSED(state);
        return renderFor(size, mode, scale);
    }
#else
    void virtual_hook(int id, void *data) override
    {
        if (id == QIconEngine::ScaledPixmapHook) {
            auto *arg = static_cast<QIconEngine::ScaledPixmapArgument *>(data);
            arg->pixmap = renderFor(arg->size, arg->mode, arg->scale);
        } else {
            QIconEngine::virtual_hook(id, data);
        }
    }
#endif

    QIconEngine *clone() const override { return new LucideIconEngine(*this); }

private:
    QColor colorForMode(QIcon::Mode mode) const
    {
        if (mode == QIcon::Disabled)
            return Manager::instance().color(Token::DisabledText);
        const QColor base = themeToken ? Manager::instance().color(*themeToken) : fixedColor;
        if (mode == QIcon::Active || mode == QIcon::Selected)
            return base.lighter(135);
        return base;
    }

    QPixmap renderFor(const QSize &size, QIcon::Mode mode, qreal dpr) const
    {
        const int px = qMax(1, qMin(size.width(), size.height()));
        return Icons::pixmap(iconName, px, colorForMode(mode), dpr);
    }

    QString iconName;
    QColor fixedColor;
    std::optional<Token> themeToken;
};

} // namespace

QPixmap pixmap(const QString &name, int px, const QColor &color, qreal dpr)
{
    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1|%2|%3|%4")
                                .arg(name, QString::number(px), color.name(QColor::HexArgb),
                                     QString::number(dpr, 'g', 4));
    auto it = cache.constFind(key);
    if (it != cache.constEnd())
        return it.value();

    QPixmap pm = render(name, px, color, dpr);
    cache.insert(key, pm);
    return pm;
}

QPixmap pixmap(const QString &name, int px, Token token, qreal dpr)
{
    return pixmap(name, px, Manager::instance().color(token), dpr);
}

QIcon icon(const QString &name, const QColor &color)
{
    return QIcon(new LucideIconEngine(name, color));
}

QIcon icon(const QString &name, Token token)
{
    return QIcon(new LucideIconEngine(name, token));
}

} // namespace Icons
} // namespace Theme
} // namespace Core
} // namespace Acheron
