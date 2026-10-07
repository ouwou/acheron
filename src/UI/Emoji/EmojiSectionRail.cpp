#include "UI/Emoji/EmojiSectionRail.hpp"

#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QWheelEvent>

#include "Core/Theme/Icons.hpp"
#include "Core/Theme/Manager.hpp"
#include "UI/Emoji/PaintedImages.hpp"

namespace Acheron {
namespace UI {

namespace {

constexpr int ItemPx = 32;
constexpr int GuildIconPx = 24;
constexpr int CategoryIconPx = 18;
constexpr int ItemRadius = 4;
constexpr int GuildIconRadius = 6;

QString categoryIconName(const QString &sectionId)
{
    using namespace Core::Theme::Icons;
    static const QHash<QString, const char *> names = {
        { QString::fromLatin1(Core::PickerSection::RecentId), Name::Clock },
        { QStringLiteral("people"), Name::Smile },
        { QStringLiteral("nature"), Name::Leaf },
        { QStringLiteral("food"), Name::Utensils },
        { QStringLiteral("activity"), Name::Gamepad },
        { QStringLiteral("travel"), Name::Car },
        { QStringLiteral("objects"), Name::Lightbulb },
        { QStringLiteral("symbols"), Name::Heart },
        { QStringLiteral("flags"), Name::Flag },
    };
    return QString::fromLatin1(names.value(sectionId));
}

} // namespace

EmojiSectionRail::EmojiSectionRail(QWidget *parent) : QWidget(parent), images(new PaintedImages(this))
{
    setFixedWidth(Width);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
}

void EmojiSectionRail::setImageSource(Core::ImageManager *imageManager, Core::Snowflake accountId)
{
    images->setSource(imageManager, accountId);
}

void EmojiSectionRail::setSections(const QList<Core::PickerSection> &sections)
{
    shownSections = sections;
    current = -1;
    hovered = -1;
    offset = 0;
    update();
}

void EmojiSectionRail::setCurrent(int section)
{
    if (current == section)
        return;
    current = section;

    if (current >= 0) {
        const int top = current * ItemPx;
        if (top < offset)
            setOffset(top);
        else if (top + ItemPx > offset + height())
            setOffset(top + ItemPx - height());
    }
    update();
}

void EmojiSectionRail::setOffset(int newOffset)
{
    const int maxOffset = qMax(0, int(shownSections.size()) * ItemPx - height());
    newOffset = qBound(0, newOffset, maxOffset);
    if (offset == newOffset)
        return;
    offset = newOffset;
    update();
}

void EmojiSectionRail::setHovered(int section)
{
    if (hovered == section)
        return;
    hovered = section;
    update();
}

int EmojiSectionRail::sectionAt(const QPoint &pos) const
{
    const int section = (pos.y() + offset) / ItemPx;
    return pos.y() + offset >= 0 && section < shownSections.size() ? section : -1;
}

QRect EmojiSectionRail::itemRect(int section) const
{
    return QRect((width() - ItemPx) / 2, section * ItemPx - offset, ItemPx, ItemPx);
}

void EmojiSectionRail::paintEvent(QPaintEvent *event)
{
    using namespace Core::Theme;
    const Manager &theme = Manager::instance();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const int first = qMax(0, (event->rect().top() + offset) / ItemPx);
    const int last = qMin(int(shownSections.size()) - 1, (event->rect().bottom() + offset) / ItemPx);
    for (int section = first; section <= last; section++) {
        const Core::PickerSection &shown = shownSections.at(section);
        const QRect rect = itemRect(section);

        if (section == current || section == hovered) {
            QColor highlight = theme.color(Token::Highlight);
            highlight.setAlpha(section == current ? 80 : 40);
            painter.setPen(Qt::NoPen);
            painter.setBrush(highlight);
            painter.drawRoundedRect(rect.adjusted(2, 2, -2, -2), ItemRadius, ItemRadius);
        }

        if (!shown.guildId.isValid()) {
            const QString iconName = categoryIconName(shown.id);
            const int inset = (ItemPx - CategoryIconPx) / 2;
            painter.drawPixmap(rect.left() + inset, rect.top() + inset, Icons::pixmap(iconName, CategoryIconPx, section == current ? Token::PrimaryText : Token::PlaceholderText, devicePixelRatioF()));
            continue;
        }

        const int inset = (ItemPx - GuildIconPx) / 2;
        const QRect iconRect(rect.left() + inset, rect.top() + inset, GuildIconPx, GuildIconPx);
        QPainterPath clip;
        clip.addRoundedRect(iconRect, GuildIconRadius, GuildIconRadius);

        const QPixmap icon = images->pixmap(shown.guildIconUrl, GuildIconPx, rect);
        if (!icon.isNull()) {
            painter.save();
            painter.setClipPath(clip);
            painter.drawPixmap(iconRect, icon);
            painter.restore();
            continue;
        }

        painter.fillPath(clip, theme.color(Token::AlternateBaseBg));
        painter.setPen(theme.color(Token::PrimaryText));
        painter.setFont(theme.font(FontRole::Ui));
        painter.drawText(iconRect, Qt::AlignCenter, shown.title.left(1).toUpper());
    }
}

void EmojiSectionRail::mouseMoveEvent(QMouseEvent *event)
{
    setHovered(sectionAt(event->pos()));
}

void EmojiSectionRail::mousePressEvent(QMouseEvent *event)
{
    const int section = sectionAt(event->pos());
    if (event->button() == Qt::LeftButton && section >= 0)
        emit sectionClicked(section);
}

void EmojiSectionRail::wheelEvent(QWheelEvent *event)
{
    constexpr int WheelStepUnits = 120;
    setOffset(offset - event->angleDelta().y() * ItemPx / WheelStepUnits);
    event->accept();
}

void EmojiSectionRail::leaveEvent(QEvent *event)
{
    setHovered(-1);
    QWidget::leaveEvent(event);
}

void EmojiSectionRail::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    setOffset(offset);
}

bool EmojiSectionRail::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const int section = sectionAt(help->pos());
        if (section >= 0)
            QToolTip::showText(help->globalPos(), shownSections.at(section).title, this, itemRect(section));
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}

} // namespace UI
} // namespace Acheron
