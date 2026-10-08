#include "UI/Emoji/ExpressionPicker.hpp"

#include <QButtonGroup>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "Core/Theme/Manager.hpp"
#include "UI/Emoji/EmojiPicker.hpp"
#include "UI/Gifs/FavoriteGifPicker.hpp"

namespace Acheron {
namespace UI {

namespace {

constexpr int CornerRadius = 8;
constexpr int AnchorSpacing = 8;
constexpr int TabRowMargin = 8;
constexpr int TabSpacing = 4;

QToolButton *makeTab(const QString &title, QWidget *parent)
{
    auto *tab = new QToolButton(parent);
    tab->setText(title);
    tab->setCheckable(true);
    tab->setAutoRaise(true);
    tab->setFocusPolicy(Qt::NoFocus);
    tab->setCursor(Qt::PointingHandCursor);
    return tab;
}

} // namespace

ExpressionPicker::ExpressionPicker(Core::ImageManager *imageManager, Core::AnimatedImageCache *animatedCache, QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setObjectName("ExpressionPicker");
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoMouseReplay);

    tabRow = new QWidget(this);
    gifTab = makeTab(tr("GIFs"), tabRow);
    emojiTab = makeTab(tr("Emoji"), tabRow);

    auto *tabGroup = new QButtonGroup(this);
    tabGroup->addButton(gifTab);
    tabGroup->addButton(emojiTab);

    auto *tabLayout = new QHBoxLayout(tabRow);
    tabLayout->setContentsMargins(TabRowMargin, TabRowMargin, TabRowMargin, 0);
    tabLayout->setSpacing(TabSpacing);
    tabLayout->addWidget(gifTab);
    tabLayout->addWidget(emojiTab);
    tabLayout->addStretch();

    gifPage = new FavoriteGifPicker(imageManager, this);
    gifPage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    emojiPage = new EmojiPicker(imageManager, animatedCache, this);

    pages = new QStackedWidget(this);
    pages->addWidget(gifPage);
    pages->addWidget(emojiPage);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    layout->addWidget(tabRow);
    layout->addWidget(pages);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    gifPage->searchField()->installEventFilter(this);
    emojiPage->searchField()->installEventFilter(this);

    connect(gifTab, &QToolButton::clicked, this, [this] { showTab(Tab::Gifs); });
    connect(emojiTab, &QToolButton::clicked, this, [this] { showTab(Tab::Emoji); });
    connect(gifPage, &FavoriteGifPicker::gifPicked, this, [this](const QString &url) {
        emit gifPicked(url);
        hide();
    });
    connect(emojiPage, &EmojiPicker::emojiPicked, this, [this](const Core::PickerEmoji &emoji, bool pickerStaysOpen) {
        emit emojiPicked(emoji, pickerStaysOpen);
        if (!pickerStaysOpen)
            hide();
    });
}

void ExpressionPicker::setAnimationEnabled(bool enabled)
{
    emojiPage->setAnimationEnabled(enabled);
}

GifPlayback *ExpressionPicker::gifPlayback() const
{
    return gifPage->gifPlayback();
}

void ExpressionPicker::openForReaction(Core::EmojiManager *emojiManager, Core::Snowflake accountId, Core::Snowflake channelId, const QRect &globalAnchor)
{
    tabRow->hide();
    emojiPage->prepareFor(emojiManager, accountId, channelId, Core::EmojiIntention::Reaction);
    pages->setCurrentWidget(emojiPage);

    placeAt(globalAnchor, Placement::BesideAnchor);
    show();
    emojiPage->searchField()->setFocus();
}

void ExpressionPicker::openForChat(Core::EmojiManager *emojiManager, Core::FavoriteGifs *favoriteGifs, Core::Snowflake accountId, Core::Snowflake channelId, Tab tab, const QRect &globalAnchor)
{
    tabRow->show();
    emojiPage->prepareFor(emojiManager, accountId, channelId, Core::EmojiIntention::Chat);
    gifPage->prepareFor(favoriteGifs, accountId);

    placeAt(globalAnchor, Placement::AboveAnchor);
    show();
    showTab(tab);
}

void ExpressionPicker::showTab(Tab tab)
{
    (tab == Tab::Gifs ? gifTab : emojiTab)->setChecked(true);
    pages->setCurrentWidget(tab == Tab::Gifs ? static_cast<QWidget *>(gifPage) : emojiPage);
    currentSearchField()->setFocus();
    emit chatTabShown(tab);
}

QLineEdit *ExpressionPicker::currentSearchField() const
{
    return pages->currentWidget() == gifPage ? gifPage->searchField() : emojiPage->searchField();
}

void ExpressionPicker::onTabShortcut(Tab tab)
{
    const bool tabbed = !tabRow->isHidden();
    const Tab shown = pages->currentWidget() == gifPage ? Tab::Gifs : Tab::Emoji;
    if (tabbed && shown != tab)
        showTab(tab);
    else if (shown == tab)
        hide();
}

void ExpressionPicker::placeAt(const QRect &globalAnchor, Placement placement)
{
    layout()->activate();
    const QSize size = sizeHint();

    QScreen *screen = QGuiApplication::screenAt(globalAnchor.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(globalAnchor.topLeft(), size);

    QPoint preferred(globalAnchor.right() + 1 - size.width(), globalAnchor.top() - AnchorSpacing - size.height());
    if (placement == Placement::BesideAnchor) {
        const int leftOfAnchor = globalAnchor.left() - AnchorSpacing - size.width();
        preferred = QPoint(leftOfAnchor >= available.left() ? leftOfAnchor : globalAnchor.right() + 1 + AnchorSpacing, globalAnchor.top());
    }

    const int x = qBound(available.left(), preferred.x(), qMax(available.left(), available.right() + 1 - size.width()));
    const int y = qBound(available.top(), preferred.y(), qMax(available.top(), available.bottom() + 1 - size.height()));
    move(x, y);
}

bool ExpressionPicker::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress)
        return QFrame::eventFilter(watched, event);

    const auto *key = static_cast<QKeyEvent *>(event);
    if (key->modifiers() != Qt::ControlModifier || (key->key() != Qt::Key_E && key->key() != Qt::Key_G))
        return QFrame::eventFilter(watched, event);

    onTabShortcut(key->key() == Qt::Key_G ? Tab::Gifs : Tab::Emoji);
    return true;
}

void ExpressionPicker::paintEvent(QPaintEvent *)
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

void ExpressionPicker::hideEvent(QHideEvent *event)
{
    QFrame::hideEvent(event);
    emit closed();
}

} // namespace UI
} // namespace Acheron
