#pragma once

#include <QAbstractListModel>
#include <QFrame>
#include <QList>
#include <QStyledItemDelegate>

#include <functional>
#include <optional>

#include "Core/Emoji/EmojiMatch.hpp"
#include "Core/Snowflake.hpp"

class QKeyEvent;
class QLabel;
class QListView;
class QTextEdit;

namespace Acheron {

namespace Core {
class ImageManager;
}

namespace UI {

class EmojiAutocompleteModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit EmojiAutocompleteModel(QObject *parent = nullptr);

    void setMatches(const QList<Core::EmojiMatch> &newMatches);
    [[nodiscard]] const QList<Core::EmojiMatch> &matches() const { return items; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;

private:
    QList<Core::EmojiMatch> items;
};

class EmojiAutocompleteDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit EmojiAutocompleteDelegate(QObject *parent = nullptr);

    void setImageManager(Core::ImageManager *manager, Core::Snowflake account);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    Core::ImageManager *imageManager = nullptr;
    Core::Snowflake accountId;
};

// view
class EmojiAutocompletePopup : public QFrame
{
    Q_OBJECT
public:
    explicit EmojiAutocompletePopup(QWidget *parent = nullptr);

    void setImageManager(Core::ImageManager *manager, Core::Snowflake account);
    void setResults(const QList<Core::EmojiMatch> &results, const QString &query);
    void moveSelection(int delta);
    [[nodiscard]] std::optional<Core::EmojiMatch> currentMatch() const;
    void showAbove(QWidget *anchor);

signals:
    void selected(const Core::EmojiMatch &match);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void applyTheme();
    void emitSelected(const QModelIndex &index);

    QLabel *header;
    QListView *listView;
    EmojiAutocompleteModel *model;
    EmojiAutocompleteDelegate *delegate;
    QMetaObject::Connection imageConnection;
};

// attaches itself to the given QTextEdit
class EmojiAutocomplete : public QObject
{
    Q_OBJECT
public:
    using Provider = std::function<QList<Core::EmojiMatch>(const QString &query)>;

    EmojiAutocomplete(QTextEdit *textEdit, QWidget *anchorWidget);

    void setProvider(Provider newProvider);
    void setImageManager(Core::ImageManager *manager, Core::Snowflake account);
    void cancel();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refresh();
    void accept(const Core::EmojiMatch &match);
    bool handleKey(const QKeyEvent *key);
    void showPopup();
    void watchWindow();

    QTextEdit *editor;
    QWidget *anchor;
    QWidget *anchorWindow = nullptr;
    EmojiAutocompletePopup *popup;
    Provider provider;
    // position of query word last given to provider
    int wordStart = -1;
    QString query;
    int dismissedWordStart = -1;
};

} // namespace UI
} // namespace Acheron
