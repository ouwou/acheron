#include "EmojiAutocompletePopup.hpp"

#include <QCursor>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTextBlock>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#include "Core/ImageManager.hpp"
#include "Core/Theme/Manager.hpp"

namespace Acheron {
namespace UI {

static constexpr int kPadX = 12;
static constexpr int kGap = 8;
static constexpr int kEmojiPx = 20;
static constexpr int kEmojiGlyphPx = 18;
static constexpr int kAnchorGap = 4;
static constexpr int kRowHeight = 30;
static constexpr int kMinWidth = 160;
static constexpr int kMaxVisibleRows = 10;
static constexpr int kMinQueryChars = 2;

EmojiAutocompleteModel::EmojiAutocompleteModel(QObject *parent) : QAbstractListModel(parent) {}

void EmojiAutocompleteModel::setMatches(const QList<Core::EmojiMatch> &newMatches)
{
    beginResetModel();
    items = newMatches;
    endResetModel();
}

int EmojiAutocompleteModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(items.size());
}

QVariant EmojiAutocompleteModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= items.size())
        return {};
    if (role == Qt::DisplayRole)
        return items.at(index.row()).displayLabel;
    return {};
}

EmojiAutocompleteDelegate::EmojiAutocompleteDelegate(QObject *parent) : QStyledItemDelegate(parent)
{
}

void EmojiAutocompleteDelegate::setImageManager(Core::ImageManager *manager, Core::Snowflake account)
{
    imageManager = manager;
    accountId = account;
}

void EmojiAutocompleteDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                      const QModelIndex &index) const
{
    const auto *listModel = qobject_cast<const EmojiAutocompleteModel *>(index.model());
    if (!listModel || index.row() < 0 || index.row() >= listModel->matches().size())
        return;
    const Core::EmojiMatch &match = listModel->matches().at(index.row());

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect r = option.rect;
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    if (selected || hovered) {
        QPainterPath path;
        path.addRoundedRect(r.adjusted(4, 1, -4, -1), 4, 4);
        QColor bg = option.palette.color(QPalette::Active, QPalette::Highlight);
        if (!selected)
            bg.setAlpha(90);
        painter->fillPath(path, bg);
    }

    const QColor textColor = selected ? option.palette.color(QPalette::Active, QPalette::HighlightedText)
                                      : option.palette.color(QPalette::Active, QPalette::Text);

    const QRect emojiRect(r.left() + kPadX,
                          r.top() + (r.height() - kEmojiPx) / 2,
                          kEmojiPx,
                          kEmojiPx);
    if (match.isCustom()) {
        if (imageManager && !match.imageUrl.isEmpty()) {
            const QPixmap pixmap =
                    imageManager->get(match.imageUrl, QSize(kEmojiPx, kEmojiPx), accountId);
            if (!pixmap.isNull())
                painter->drawPixmap(emojiRect, pixmap);
        }
    } else if (!match.surrogates.isEmpty()) {
        QFont emojiFont = Core::Theme::Manager::instance().font(Core::Theme::FontRole::Message);
        emojiFont.setPixelSize(kEmojiGlyphPx);
        painter->setFont(emojiFont);
        painter->setPen(textColor);
        painter->drawText(emojiRect, Qt::AlignCenter, match.surrogates);
    }

    const int textLeft = emojiRect.right() + kGap;
    const QRect textRect(textLeft, r.top(), r.right() - kPadX - textLeft, r.height());
    if (textRect.width() > 0) {
        painter->setFont(option.font);
        painter->setPen(textColor);
        const QFontMetrics fm(option.font);
        painter->drawText(textRect,
                          Qt::AlignLeft | Qt::AlignVCenter,
                          fm.elidedText(match.displayLabel, Qt::ElideRight, textRect.width()));
    }

    painter->restore();
}

QSize EmojiAutocompleteDelegate::sizeHint(const QStyleOptionViewItem &option,
                                          const QModelIndex &) const
{
    return QSize(qMax(option.rect.width(), kMinWidth), kRowHeight);
}

EmojiAutocompletePopup::EmojiAutocompletePopup(QWidget *parent)
    : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus)
{
    setObjectName("EmojiAutocompletePopup");
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 6, 0, 6);
    layout->setSpacing(2);

    header = new QLabel(this);
    header->setContentsMargins(kPadX, 0, kPadX, 0);
    header->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(header, 0);

    model = new EmojiAutocompleteModel(this);
    delegate = new EmojiAutocompleteDelegate(this);

    listView = new QListView(this);
    listView->setModel(model);
    listView->setItemDelegate(delegate);
    listView->setFocusPolicy(Qt::NoFocus);
    listView->setFrameShape(QFrame::NoFrame);
    listView->setUniformItemSizes(true);
    listView->setSelectionMode(QAbstractItemView::SingleSelection);
    listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    listView->setMouseTracking(true);
    listView->viewport()->setAutoFillBackground(false);
    layout->addWidget(listView, 1);

    connect(listView, &QListView::clicked, this, &EmojiAutocompletePopup::emitSelected);
    connect(&Core::Theme::Manager::instance(), &Core::Theme::Manager::themeChanged, this, &EmojiAutocompletePopup::applyTheme);
    connect(&Core::Theme::Manager::instance(), &Core::Theme::Manager::metricsChanged, this, &EmojiAutocompletePopup::applyTheme);

    applyTheme();
}

void EmojiAutocompletePopup::applyTheme()
{
    const Core::Theme::Manager &theme = Core::Theme::Manager::instance();

    QFont headerFont = theme.font(Core::Theme::FontRole::Ui);
    if (headerFont.pointSizeF() > 0)
        headerFont.setPointSizeF(headerFont.pointSizeF() * 0.85);
    headerFont.setBold(true);
    header->setFont(headerFont);

    QPalette headerPalette = header->palette();
    headerPalette.setColor(QPalette::WindowText, theme.color(Core::Theme::Token::PlaceholderText));
    header->setPalette(headerPalette);

    update();
}

void EmojiAutocompletePopup::setImageManager(Core::ImageManager *manager, Core::Snowflake account)
{
    QObject::disconnect(imageConnection);
    delegate->setImageManager(manager, account);
    if (!manager)
        return;

    imageConnection = connect(manager, &Core::ImageManager::imageFetched, this,
                              [this](const QUrl &url, const QSize &, const QPixmap &) {
                                  if (!isVisible())
                                      return;
                                  const QList<Core::EmojiMatch> &matches = model->matches();
                                  const bool shown = std::any_of(
                                          matches.begin(), matches.end(),
                                          [&url](const Core::EmojiMatch &match) { return match.imageUrl == url; });
                                  if (shown)
                                      listView->viewport()->update();
                              });
}

void EmojiAutocompletePopup::setResults(const QList<Core::EmojiMatch> &results, const QString &query)
{
    model->setMatches(results);
    header->setText(tr("EMOJI MATCHING :%1").arg(query));

    const int visibleRows = qBound(1, static_cast<int>(results.size()), kMaxVisibleRows);
    listView->setFixedHeight(visibleRows * kRowHeight);

    if (!results.isEmpty()) {
        const QModelIndex first = model->index(0, 0);
        listView->setCurrentIndex(first);
        listView->scrollTo(first, QAbstractItemView::PositionAtTop);
    }
}

void EmojiAutocompletePopup::moveSelection(int delta)
{
    const int count = model->rowCount();
    if (count == 0)
        return;

    const QModelIndex current = listView->currentIndex();
    const int row = current.isValid() ? current.row() : 0;
    const QModelIndex target = model->index(((row + delta) % count + count) % count, 0);
    listView->setCurrentIndex(target);
    listView->scrollTo(target);
}

std::optional<Core::EmojiMatch> EmojiAutocompletePopup::currentMatch() const
{
    const QModelIndex current = listView->currentIndex();
    if (!current.isValid() || current.row() >= model->matches().size())
        return std::nullopt;
    return model->matches().at(current.row());
}

void EmojiAutocompletePopup::showAbove(QWidget *anchor)
{
    if (!anchor || model->rowCount() == 0)
        return;

    const int width = qMax(anchor->width(), kMinWidth);
    setFixedWidth(width);
    layout()->activate();
    const int height = sizeHint().height();
    setFixedHeight(height);

    const QPoint anchorPos = anchor->mapToGlobal(QPoint(0, 0));
    int x = anchorPos.x();
    int y = anchorPos.y() - height - kAnchorGap;

    QScreen *screen = QGuiApplication::screenAt(anchorPos);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        x = qBound(avail.left(), x, qMax(avail.left(), avail.right() - width + 1));
        if (y < avail.top())
            y = anchorPos.y() + anchor->height() + kAnchorGap;
        y = qBound(avail.top(), y, qMax(avail.top(), avail.bottom() - height + 1));
    }

    move(x, y);
    if (!isVisible())
        show();
    raise();
}

void EmojiAutocompletePopup::emitSelected(const QModelIndex &index)
{
    if (!index.isValid() || index.row() >= model->matches().size())
        return;
    emit selected(model->matches().at(index.row()));
}

void EmojiAutocompletePopup::paintEvent(QPaintEvent *)
{
    using namespace Core::Theme;
    const Manager &theme = Manager::instance();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    painter.fillPath(path, theme.color(Token::BaseBg));
    painter.setPen(theme.color(Token::Divider));
    painter.drawPath(path);
}

namespace {

struct EmojiTrigger
{
    QString query;
    int wordStart;
};

std::optional<EmojiTrigger> triggerAt(const QString &line, int pos)
{
    int start = pos;
    while (start > 0 && !line.at(start - 1).isSpace())
        --start;

    const QString word = line.mid(start, pos - start);
    const bool alreadyClosed = word.indexOf(':', 1) != -1;
    if (!word.startsWith(':') || word.size() < kMinQueryChars + 1 || alreadyClosed)
        return std::nullopt;

    return EmojiTrigger{ word.mid(1), start };
}

} // namespace

EmojiAutocomplete::EmojiAutocomplete(QTextEdit *textEdit, QWidget *anchorWidget)
    : QObject(anchorWidget), editor(textEdit), anchor(anchorWidget), popup(new EmojiAutocompletePopup(anchorWidget))
{
    connect(editor, &QTextEdit::cursorPositionChanged, this, &EmojiAutocomplete::refresh);
    connect(editor, &QTextEdit::textChanged, this, &EmojiAutocomplete::refresh);
    connect(popup, &EmojiAutocompletePopup::selected, this, &EmojiAutocomplete::accept);

    editor->installEventFilter(this);
    anchor->installEventFilter(this);
}

void EmojiAutocomplete::setProvider(Provider newProvider)
{
    provider = std::move(newProvider);
    cancel();
}

void EmojiAutocomplete::setImageManager(Core::ImageManager *manager, Core::Snowflake account)
{
    popup->setImageManager(manager, account);
}

void EmojiAutocomplete::cancel()
{
    wordStart = -1;
    dismissedWordStart = -1;
    query.clear();
    popup->hide();
}

void EmojiAutocomplete::refresh()
{
    const QTextCursor cursor = editor->textCursor();
    std::optional<EmojiTrigger> trigger;
    if (provider && !cursor.hasSelection())
        trigger = triggerAt(cursor.block().text(), cursor.positionInBlock());
    if (!trigger) {
        cancel();
        return;
    }

    const int docStart = cursor.block().position() + trigger->wordStart;
    if (docStart == dismissedWordStart) {
        popup->hide();
        return;
    }
    dismissedWordStart = -1;

    if (docStart == wordStart && trigger->query == query)
        return;
    wordStart = docStart;
    query = trigger->query;

    const QList<Core::EmojiMatch> results = provider(query.toLower());
    if (results.isEmpty()) {
        popup->hide();
        return;
    }

    popup->setResults(results, query);
    showPopup();
}

void EmojiAutocomplete::accept(const Core::EmojiMatch &match)
{
    QTextCursor cursor = editor->textCursor();
    const int start = wordStart;
    const int end = cursor.position();
    cancel();
    if (start < 0 || start > end)
        return;

    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.insertText(match.insertText + " ");
    editor->setTextCursor(cursor);
    editor->setFocus();
}

bool EmojiAutocomplete::handleKey(const QKeyEvent *key)
{
    const bool ctrl = key->modifiers() & Qt::ControlModifier;

    switch (key->key()) {
    case Qt::Key_Up:
        popup->moveSelection(-1);
        return true;
    case Qt::Key_Down:
        popup->moveSelection(1);
        return true;
    case Qt::Key_P:
    case Qt::Key_N:
        if (!ctrl)
            return false;
        popup->moveSelection(key->key() == Qt::Key_P ? -1 : 1);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Tab:
    case Qt::Key_Backtab: {
        const bool isEnter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        if (isEnter && (key->modifiers() & Qt::ShiftModifier))
            return false;
        const std::optional<Core::EmojiMatch> match = popup->currentMatch();
        if (!match)
            return false;
        accept(*match);
        return true;
    }
    case Qt::Key_Escape:
        dismissedWordStart = wordStart;
        popup->hide();
        return true;
    default:
        return false;
    }
}

void EmojiAutocomplete::showPopup()
{
    watchWindow();
    popup->showAbove(anchor);
}

void EmojiAutocomplete::watchWindow()
{
    QWidget *window = anchor->window();
    if (window == anchorWindow)
        return;

    if (anchorWindow)
        anchorWindow->removeEventFilter(this);
    anchorWindow = window;
    if (anchorWindow)
        anchorWindow->installEventFilter(this);
}

bool EmojiAutocomplete::eventFilter(QObject *watched, QEvent *event)
{
    if (!popup->isVisible())
        return QObject::eventFilter(watched, event);

    if (watched == editor) {
        if (event->type() == QEvent::KeyPress && handleKey(static_cast<QKeyEvent *>(event)))
            return true;
        if (event->type() == QEvent::FocusOut && !popup->frameGeometry().contains(QCursor::pos()))
            cancel();
        return QObject::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::Move:
    case QEvent::Resize:
        popup->showAbove(anchor);
        break;
    case QEvent::Hide:
    case QEvent::WindowDeactivate:
        cancel();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace UI
} // namespace Acheron
