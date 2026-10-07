#include "UI/Emoji/EmojiPicker.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <optional>

#include "Core/Theme/Icons.hpp"
#include "Core/Theme/Manager.hpp"
#include "UI/Emoji/EmojiGridView.hpp"
#include "UI/Emoji/EmojiPainting.hpp"
#include "UI/Emoji/EmojiSectionRail.hpp"
#include "UI/Emoji/PaintedImages.hpp"

namespace Acheron {
namespace UI {

namespace {

constexpr int OuterMargin = 8;
constexpr int CornerRadius = 8;
constexpr int InspectorHeight = 40;
constexpr int InspectorEmojiPx = 28;
constexpr int InspectorGap = 8;
constexpr int AnchorSpacing = 8;
constexpr auto CollapsedSectionsKey = "emoji_picker/collapsed_sections";

} // namespace

class EmojiInspector : public QWidget
{
public:
    explicit EmojiInspector(QWidget *parent) : QWidget(parent), images(new PaintedImages(this))
    {
        setFixedHeight(InspectorHeight);
    }

    void setImageSource(Core::ImageManager *imageManager, Core::Snowflake accountId)
    {
        images->setSource(imageManager, accountId);
    }

    void inspect(const Core::PickerEmoji *emoji, const QString &guildName)
    {
        shown = emoji ? std::optional(*emoji) : std::nullopt;
        shownGuildName = guildName;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        using namespace Core::Theme;
        const Manager &theme = Manager::instance();

        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setPen(theme.color(Token::Divider));
        painter.drawLine(0, 0, width(), 0);
        if (!shown)
            return;

        const QRect emojiRect(OuterMargin, (height() - InspectorEmojiPx) / 2, InspectorEmojiPx, InspectorEmojiPx);
        drawEmojiStill(painter, emojiRect, *shown, *images, rect());

        QFont nameFont = theme.font(FontRole::Ui);
        nameFont.setBold(true);
        painter.setFont(nameFont);
        painter.setPen(theme.color(Token::PrimaryText));

        const int textLeft = emojiRect.right() + 1 + InspectorGap;
        const int textWidth = width() - OuterMargin - textLeft;
        const QString name = ":" + shown->name + ":";
        const QString guildText = shownGuildName.isEmpty() ? QString() : QCoreApplication::translate("EmojiPicker", "from %1").arg(shownGuildName);

        const QFontMetrics nameMetrics(nameFont);
        const QFontMetrics guildMetrics(theme.font(FontRole::Ui));
        const int guildWidth = guildText.isEmpty() ? 0 : qMin(guildMetrics.horizontalAdvance(guildText), textWidth / 2);
        const int nameWidth = textWidth - (guildWidth > 0 ? guildWidth + InspectorGap : 0);
        painter.drawText(QRect(textLeft, 0, nameWidth, height()), Qt::AlignLeft | Qt::AlignVCenter, nameMetrics.elidedText(name, Qt::ElideRight, nameWidth));

        if (guildWidth > 0) {
            painter.setFont(theme.font(FontRole::Ui));
            painter.setPen(theme.color(Token::PlaceholderText));
            painter.drawText(QRect(width() - OuterMargin - guildWidth, 0, guildWidth, height()), Qt::AlignRight | Qt::AlignVCenter, guildMetrics.elidedText(guildText, Qt::ElideRight, guildWidth));
        }
    }

private:
    PaintedImages *images;
    std::optional<Core::PickerEmoji> shown;
    QString shownGuildName;
};

EmojiPicker::EmojiPicker(Core::ImageManager *imageManager, Core::AnimatedImageCache *animatedCache, QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), imageManager(imageManager), animatedCache(animatedCache)
{
    setObjectName("EmojiPicker");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoMouseReplay);

    search = new QLineEdit(this);
    search->setPlaceholderText(tr("Find the perfect reaction"));
    search->setClearButtonEnabled(true);
    search->addAction(Core::Theme::Icons::icon(Core::Theme::Icons::Name::Search, Core::Theme::Token::PlaceholderText), QLineEdit::LeadingPosition);
    search->installEventFilter(this);

    rail = new EmojiSectionRail(this);
    grid = new EmojiGridView(this);
    grid->setEmptyText(tr("No emoji match your search"));
    grid->setFixedSize(grid->sizeHint());
    inspector = new EmojiInspector(this);

    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(rail);
    body->addWidget(grid);

    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(OuterMargin, OuterMargin, OuterMargin, OuterMargin);
    searchRow->addWidget(search);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    layout->addLayout(searchRow);
    layout->addLayout(body);
    layout->addWidget(inspector);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    connect(search, &QLineEdit::textChanged, this, &EmojiPicker::showSearchResults);
    connect(rail, &EmojiSectionRail::sectionClicked, this, &EmojiPicker::onSectionClicked);
    connect(grid, &EmojiGridView::activeEmojiChanged, this, &EmojiPicker::onActiveEmojiChanged);
    connect(grid, &EmojiGridView::collapsedSectionsChanged, this, &EmojiPicker::saveCollapsedSections);
    connect(grid, &EmojiGridView::topSectionChanged, this, [this](int section) {
        rail->setCurrent(searching ? -1 : section);
    });
    connect(grid, &EmojiGridView::emojiClicked, this, [this](const Core::PickerEmoji &emoji, Qt::KeyboardModifiers modifiers) {
        pick(emoji, modifiers.testFlag(Qt::ShiftModifier));
    });

    const QStringList collapsedIds = QSettings().value(CollapsedSectionsKey).toStringList();
    grid->setCollapsedSections(QSet<QString>(collapsedIds.cbegin(), collapsedIds.cend()));
}

void EmojiPicker::setAnimationEnabled(bool enabled)
{
    grid->setAnimationEnabled(enabled);
}

void EmojiPicker::openFor(Core::EmojiManager *emojiManager, Core::Snowflake accountId, Core::Snowflake channelId, const QRect &globalAnchor)
{
    emojis = emojiManager;
    this->channelId = channelId;

    grid->setImageSources(imageManager, animatedCache, accountId);
    rail->setImageSource(imageManager, accountId);
    inspector->setImageSource(imageManager, accountId);

    browseSections = emojiManager->reactionPickerSections(channelId);
    guildNames.clear();
    for (const Core::PickerSection &section : browseSections) {
        if (section.guildId.isValid())
            guildNames.insert(section.guildId, section.title);
    }
    rail->setSections(browseSections);

    const QSignalBlocker blocker(search);
    search->clear();
    showBrowseSections();

    placeBeside(globalAnchor);
    show();
    search->setFocus();
}

void EmojiPicker::placeBeside(const QRect &globalAnchor)
{
    layout()->activate();
    const QSize size = sizeHint();

    QScreen *screen = QGuiApplication::screenAt(globalAnchor.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(globalAnchor.topLeft(), size);

    int x = globalAnchor.left() - AnchorSpacing - size.width();
    if (x < available.left())
        x = globalAnchor.right() + 1 + AnchorSpacing;
    x = qBound(available.left(), x, qMax(available.left(), available.right() + 1 - size.width()));
    const int y = qBound(available.top(), globalAnchor.top(), qMax(available.top(), available.bottom() + 1 - size.height()));
    move(x, y);
}

void EmojiPicker::showBrowseSections()
{
    searching = false;
    grid->setSections(browseSections);
    grid->activateFirst();
}

void EmojiPicker::showSearchResults(const QString &text)
{
    QString query = text.trimmed();
    if (query.startsWith(':'))
        query.remove(0, 1);
    if (query.endsWith(':'))
        query.chop(1);

    if (query.isEmpty() || !emojis) {
        showBrowseSections();
        return;
    }

    searching = true;
    const QList<Core::PickerEmoji> results = emojis->searchReactions(query, channelId);
    QList<Core::PickerSection> sections;
    if (!results.isEmpty()) {
        Core::PickerSection section;
        section.emojis = results;
        sections.append(section);
    }
    grid->setSections(sections);
    grid->activateFirst();
    rail->setCurrent(-1);
}

void EmojiPicker::pick(const Core::PickerEmoji &emoji, bool keepOpen)
{
    emit emojiPicked(emoji);
    if (!keepOpen)
        hide();
}

void EmojiPicker::onActiveEmojiChanged()
{
    const Core::PickerEmoji *emoji = grid->activeEmoji();
    search->setPlaceholderText(emoji ? ":" + emoji->name + ":" : tr("Find the perfect reaction"));
    inspector->inspect(emoji, emoji && emoji->isCustom() ? guildNames.value(emoji->guildId) : QString());
}

void EmojiPicker::onSectionClicked(int section)
{
    if (searching)
        search->clear();
    grid->scrollToSection(section);
}

void EmojiPicker::saveCollapsedSections()
{
    const QSet<QString> &collapsedIds = grid->collapsedSections();
    QSettings().setValue(CollapsedSectionsKey, QStringList(collapsedIds.cbegin(), collapsedIds.cend()));
}

bool EmojiPicker::handleSearchKey(const QKeyEvent *key)
{
    switch (key->key()) {
    case Qt::Key_Up:
        grid->moveActive(0, -1);
        return true;
    case Qt::Key_Down:
        grid->moveActive(0, 1);
        return true;
    case Qt::Key_Left:
        if (grid->activeIsFirst())
            return false;
        grid->moveActive(-1, 0);
        return true;
    case Qt::Key_Right:
        grid->moveActive(1, 0);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!key->isAutoRepeat()) {
            if (const Core::PickerEmoji *emoji = grid->activeEmoji())
                pick(*emoji, key->modifiers().testFlag(Qt::ShiftModifier));
        }
        return true;
    default:
        return false;
    }
}

bool EmojiPicker::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == search && event->type() == QEvent::KeyPress && handleSearchKey(static_cast<QKeyEvent *>(event)))
        return true;
    return QFrame::eventFilter(watched, event);
}

void EmojiPicker::paintEvent(QPaintEvent *)
{
    using namespace Core::Theme;
    const Manager &theme = Manager::instance();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), CornerRadius, CornerRadius);
    painter.fillPath(path, theme.color(Token::BaseBg));
    painter.setPen(theme.color(Token::Divider));
    painter.drawPath(path);
}

void EmojiPicker::hideEvent(QHideEvent *event)
{
    QFrame::hideEvent(event);
    emit closed();
}

} // namespace UI
} // namespace Acheron
