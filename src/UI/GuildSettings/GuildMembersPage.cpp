#include "GuildMembersPage.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "Core/ClientInstance.hpp"
#include "Core/ImageManager.hpp"
#include "Core/RoleHierarchy.hpp"
#include "Core/Theme/Manager.hpp"
#include "UI/Dialogs/BasePopup.hpp"
#include "UI/Dialogs/ConfirmPopup.hpp"
#include "UI/Dialogs/UserProfilePopup.hpp"

#include "GuildSettingsSection.hpp"
#include "MemberModeration.hpp"

namespace Acheron {
namespace UI {

namespace {

enum Column {
    NameColumn,
    MemberSinceColumn,
    JoinedDiscordColumn,
    JoinMethodColumn,
    RolesColumn,
    SignalsColumn,
    ColumnCount,
};

constexpr int UserIdRole = Qt::UserRole;
constexpr int TagRole = Qt::UserRole + 1;
constexpr int NameColorRole = Qt::UserRole + 2;
constexpr QSize AvatarSize(32, 32);
constexpr int PageLimit = 250;
constexpr int MaxIndexAttempts = 4;
constexpr int SearchDebounceMs = 300;
constexpr qint64 UnusualDmWindowSecs = 2 * 86400;

constexpr qint64 Minute = 60;
constexpr qint64 Hour = 3600;
constexpr qint64 Day = 86400;
constexpr qint64 Month = 30 * Day;
constexpr qint64 Year = 360 * Day;

QString countText(qint64 count, const QString &one, const QString &other)
{
    return count == 1 ? one : other.arg(count);
}

QString sinceText(qint64 seconds)
{
    if (seconds < Month)
        return countText(seconds / Day, GuildMembersPage::tr("1 day ago"), GuildMembersPage::tr("%1 days ago"));
    if (seconds < Year)
        return countText(seconds / Month, GuildMembersPage::tr("1 month ago"), GuildMembersPage::tr("%1 months ago"));
    return countText(seconds / Year, GuildMembersPage::tr("1 year ago"), GuildMembersPage::tr("%1 years ago"));
}

QString memberSinceText(const QDateTime &joinedAt)
{
    const qint64 seconds = qMax<qint64>(0, joinedAt.secsTo(QDateTime::currentDateTimeUtc()));
    if (seconds < Minute)
        return GuildMembersPage::tr("just now");
    if (seconds < Hour)
        return countText(seconds / Minute, GuildMembersPage::tr("1 min ago"), GuildMembersPage::tr("%1 mins ago"));
    if (seconds < Day)
        return countText(seconds / Hour, GuildMembersPage::tr("1 hr ago"), GuildMembersPage::tr("%1 hrs ago"));
    return sinceText(seconds);
}

QString accountAgeText(const QDateTime &createdAt)
{
    const qint64 seconds = qMax<qint64>(0, createdAt.secsTo(QDateTime::currentDateTimeUtc()));
    if (seconds < Day)
        return GuildMembersPage::tr("<1 day ago");
    return sinceText(seconds);
}

QString fullDate(const QDateTime &time)
{
    return QLocale::system().toString(time.toLocalTime(), QStringLiteral("MMM d, yyyy, h:mm AP"));
}

Core::Snowflake userIdOf(const Discord::MemberSearchResult &result)
{
    const Discord::Member &member = result.member.get();
    return member.user.hasValue() ? member.user->id.get() : Core::Snowflake();
}

class MemberNameDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString name = opt.text;
        const QIcon avatar = opt.icon;
        opt.text.clear();
        opt.icon = QIcon();
        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const QRect area = opt.rect.adjusted(6, 0, -6, 0);
        const QRect avatarRect(area.left(), area.center().y() - AvatarSize.height() / 2 + 1, AvatarSize.width(), AvatarSize.height());
        avatar.paint(painter, avatarRect);

        const int textLeft = avatarRect.right() + 10;
        const int textWidth = qMax(0, area.right() - textLeft);
        QFont nameFont = opt.font;
        nameFont.setBold(true);
        const QFontMetrics nameMetrics(nameFont);
        const QFontMetrics tagMetrics(opt.font);
        const int top = area.center().y() - (nameMetrics.height() + tagMetrics.height()) / 2 + 1;
        const bool selected = opt.state & QStyle::State_Selected;
        const QColor nameColor = index.data(NameColorRole).value<QColor>();

        painter->save();
        painter->setFont(nameFont);
        painter->setPen(nameColor.isValid() && !selected ? nameColor : opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(QRect(textLeft, top, textWidth, nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          nameMetrics.elidedText(name, Qt::ElideRight, textWidth));
        painter->setFont(opt.font);
        painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
        painter->drawText(QRect(textLeft, top + nameMetrics.height(), textWidth, tagMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          tagMetrics.elidedText(index.data(TagRole).toString(), Qt::ElideRight, textWidth));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), AvatarSize.height() + 12));
        return size;
    }
};

class PruneDialog : public BasePopup
{
public:
    PruneDialog(Core::ClientInstance *instance, Core::Snowflake guildId, const QString &guildName,
                const QList<Discord::Role> &roles, QWidget *parent)
        : BasePopup(parent), instance(instance), guildId(guildId)
    {
        setAttribute(Qt::WA_DeleteOnClose);

        auto *layout = new QVBoxLayout(getContainer());
        layout->setSpacing(8);
        layout->setContentsMargins(24, 24, 24, 24);

        auto *title = new QLabel(tr("Prune Members — %1").arg(guildName), getContainer());
        title->setTextFormat(Qt::PlainText);
        QFont titleFont = title->font();
        titleFont.setBold(true);
        titleFont.setPointSize(titleFont.pointSize() + 2);
        title->setFont(titleFont);
        title->setWordWrap(true);
        layout->addWidget(title);

        layout->addWidget(fieldLabel(tr("Last Seen")));
        days = new QComboBox(getContainer());
        days->addItem(tr("more than 7 days ago"), 7);
        days->addItem(tr("more than 30 days ago"), 30);
        layout->addWidget(days);

        if (!roles.isEmpty()) {
            layout->addWidget(fieldLabel(tr("Also include members with these roles")));
            roleList = new QListWidget(getContainer());
            for (const Discord::Role &role : roles) {
                auto *item = new QListWidgetItem(role.name.get(), roleList);
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Unchecked);
                item->setData(Qt::UserRole, QVariant::fromValue<quint64>(role.id.get()));
                if (role.hasColor())
                    item->setIcon(GuildSettingsPage::colorDot(role.getColor()));
            }
            roleList->setMaximumHeight(roleList->sizeHintForRow(0) * 6 + 2 * roleList->frameWidth());
            layout->addWidget(roleList);
            connect(roleList, &QListWidget::itemChanged, this, [this]() { requestEstimate(); });
        }

        estimateLabel = new QLabel(getContainer());
        estimateLabel->setWordWrap(true);
        estimateLabel->setTextFormat(Qt::RichText);
        layout->addSpacing(4);
        layout->addWidget(estimateLabel);

        auto *buttons = new QDialogButtonBox(getContainer());
        buttons->addButton(QDialogButtonBox::Cancel);
        buttons->addButton(tr("Prune"), QDialogButtonBox::AcceptRole)->setDefault(true);
        connect(buttons, &QDialogButtonBox::accepted, this, &PruneDialog::prune);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);

        connect(days, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { requestEstimate(); });
        connect(instance->discord(), &Discord::Client::guildPruneUpdated, this, [this](const Discord::GuildPruneUpdate &event) { onPruneUpdate(event); });
        requestEstimate();
    }

private:
    struct Estimate
    {
        int count = 0;
        bool finished = false;
    };

    QLabel *fieldLabel(const QString &text) const
    {
        auto *label = new QLabel(text, getContainer());
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        return label;
    }

    static QString keyFor(int dayCount, QList<Core::Snowflake> roleIds)
    {
        std::sort(roleIds.begin(), roleIds.end());
        QStringList parts{ QString::number(dayCount) };
        for (Core::Snowflake roleId : roleIds)
            parts.append(QString::number(quint64(roleId)));
        return parts.join(QLatin1Char(':'));
    }

    [[nodiscard]] QList<Core::Snowflake> selectedRoles() const
    {
        QList<Core::Snowflake> selected;
        if (!roleList)
            return selected;
        for (int row = 0; row < roleList->count(); row++)
            if (roleList->item(row)->checkState() == Qt::Checked)
                selected.append(Core::Snowflake(roleList->item(row)->data(Qt::UserRole).toULongLong()));
        return selected;
    }

    [[nodiscard]] int selectedDays() const { return days->currentData().toInt(); }

    void requestEstimate()
    {
        updateEstimateText();
        if (!instance || estimates.contains(keyFor(selectedDays(), selectedRoles())))
            return;
        QPointer<PruneDialog> self(this);
        instance->discord()->requestPruneCount(guildId, selectedDays(), selectedRoles(), [self](const Core::Result<void> &result) {
            if (self && !result.success())
                self->estimateLabel->setText(result.error.toHtmlEscaped());
        });
    }

    void onPruneUpdate(const Discord::GuildPruneUpdate &event)
    {
        if (event.guildId.get() != guildId || !event.isPreview.valueOr(false))
            return;
        const QString key = keyFor(event.days.valueOr(0), event.includeRoles.valueOr({}));
        auto existing = estimates.find(key);
        if (existing != estimates.end() && existing->finished)
            return;
        estimates.insert(key, { event.pruneCount.valueOr(0), event.isFinished.valueOr(true) });
        updateEstimateText();
    }

    void updateEstimateText()
    {
        const auto found = estimates.constFind(keyFor(selectedDays(), selectedRoles()));
        QString members;
        if (found == estimates.constEnd()) {
            members = tr("[calculating...] members");
        } else {
            const QString count = countText(found->count, tr("1 member"), tr("%1 members"));
            members = found->finished ? count : tr("[calculating...] %1").arg(count);
        }
        const QString dayText = countText(selectedDays(), tr("1 day"), tr("%1 days"));
        const QString rest = selectedRoles().isEmpty() ? tr("and are not assigned to any roles. They can rejoin the server using a new invite.")
                                                       : tr("and are assigned to just the roles you've selected. Members who are not assigned to any roles "
                                                            "are still included. They can rejoin the server using a new invite.");
        estimateLabel->setText(tr("Pruning will kick <b>%1</b> who have not been seen on Discord in <b>%2</b> %3")
                                       .arg(members.toHtmlEscaped(), dayText.toHtmlEscaped(), rest.toHtmlEscaped()));
    }

    void prune()
    {
        if (instance) {
            QPointer<QWidget> parent(parentWidget());
            instance->discord()->pruneMembers(guildId, selectedDays(), selectedRoles(), [parent](const Core::Result<void> &result) {
                if (parent && !result.success())
                    GuildSettingsPage::showError(parent, result.error);
            });
        }
        accept();
    }

    QPointer<Core::ClientInstance> instance;
    Core::Snowflake guildId;
    QComboBox *days;
    QListWidget *roleList = nullptr;
    QLabel *estimateLabel;
    QHash<QString, Estimate> estimates;
};

} // namespace

GuildMembersPage::GuildMembersPage(Core::ClientInstance *instance, Core::ImageManager *images, Core::Snowflake guildId,
                                   QWidget *parent)
    : GuildSettingsPage(instance, images, guildId, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 16);
    layout->addWidget(makeTitle(tr("Server Members"), this));
    permissionNotice = makePermissionNotice(tr("Listing the members Acheron has already loaded. Searching the whole server "
                                               "needs a permission to manage members."),
                                            this);
    layout->addWidget(permissionNotice);

    auto *toolbar = new QHBoxLayout();
    sectionTitle = makeFieldLabel(tr("Recent Members"), this);
    toolbar->addWidget(sectionTitle);
    toolbar->addStretch();
    searchEdit = new QLineEdit(this);
    searchEdit->setPlaceholderText(tr("Search by username or id"));
    searchEdit->setClearButtonEnabled(true);
    searchEdit->setMinimumWidth(220);
    toolbar->addWidget(searchEdit);

    sortButton = new QPushButton(tr("Sort"), this);
    auto *sortMenu = new QMenu(sortButton);
    auto *sortGroup = new QActionGroup(sortMenu);
    const QList<QPair<QString, Discord::MemberSearchQuery::Sort>> sorts = {
        { tr("Member Since (Newest first)"), Discord::MemberSearchQuery::Sort::JoinedNewest },
        { tr("Member Since (Oldest first)"), Discord::MemberSearchQuery::Sort::JoinedOldest },
        { tr("Joined Discord (Newest first)"), Discord::MemberSearchQuery::Sort::UserIdDescending },
        { tr("Joined Discord (Oldest first)"), Discord::MemberSearchQuery::Sort::UserIdAscending },
    };
    for (const auto &option : sorts) {
        QAction *action = sortMenu->addAction(option.first);
        action->setCheckable(true);
        action->setChecked(option.second == Discord::MemberSearchQuery::Sort::JoinedNewest);
        sortGroup->addAction(action);
        const auto chosen = option.second;
        connect(action, &QAction::triggered, this, [this, chosen]() {
            sort = chosen;
            search(false);
        });
    }
    sortButton->setMenu(sortMenu);
    toolbar->addWidget(sortButton);

    timedOutButton = new QPushButton(tr("Timed Out"), this);
    timedOutButton->setCheckable(true);
    timedOutButton->setToolTip(tr("Only show members who are timed out"));
    toolbar->addWidget(timedOutButton);

    pruneButton = new QPushButton(tr("Prune"), this);
    pruneButton->setAccessibleName(tr("Prune Members"));
    QPalette prunePalette = pruneButton->palette();
    prunePalette.setColor(QPalette::ButtonText, Core::Theme::Manager::instance().color(Core::Theme::Token::ChatError));
    pruneButton->setPalette(prunePalette);
    toolbar->addWidget(pruneButton);
    layout->addLayout(toolbar);

    newMembersBar = new QWidget(this);
    auto *newMembersLayout = new QHBoxLayout(newMembersBar);
    newMembersLayout->setContentsMargins(0, 0, 0, 0);
    newMembersLabel = new QLabel(newMembersBar);
    newMembersLayout->addWidget(newMembersLabel, 1);
    auto *viewNewButton = new QPushButton(tr("View New Members"), newMembersBar);
    newMembersLayout->addWidget(viewNewButton);
    newMembersBar->hide();
    layout->addWidget(newMembersBar);

    states = new QStackedWidget(this);
    table = new QTreeWidget(states);
    table->setColumnCount(ColumnCount);
    table->setHeaderLabels({ tr("Name"), tr("Member Since"), tr("Joined Discord"), tr("Join Method"), tr("Roles"), tr("Signals") });
    table->setRootIsDecorated(false);
    table->setUniformRowHeights(true);
    table->setIconSize(AvatarSize);
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    table->setItemDelegateForColumn(NameColumn, new MemberNameDelegate(table));
    table->header()->setStretchLastSection(true);
    table->header()->setSectionResizeMode(NameColumn, QHeaderView::Interactive);
    table->setColumnWidth(NameColumn, 230);
    table->setColumnWidth(MemberSinceColumn, 110);
    table->setColumnWidth(JoinedDiscordColumn, 110);
    table->setColumnWidth(JoinMethodColumn, 130);
    table->setColumnWidth(RolesColumn, 150);
    states->addWidget(table);

    messageLabel = new QLabel(states);
    messageLabel->setTextFormat(Qt::PlainText);
    messageLabel->setAlignment(Qt::AlignCenter);
    messageLabel->setWordWrap(true);
    messageLabel->setForegroundRole(QPalette::PlaceholderText);
    states->addWidget(messageLabel);
    layout->addWidget(states, 1);

    auto *footer = new QHBoxLayout();
    footerLabel = new QLabel(this);
    footerLabel->setTextFormat(Qt::RichText);
    footer->addWidget(footerLabel, 1);
    loadMoreButton = new QPushButton(tr("Load more"), this);
    loadMoreButton->hide();
    footer->addWidget(loadMoreButton);
    layout->addLayout(footer);

    searchDebounce = new QTimer(this);
    searchDebounce->setSingleShot(true);
    searchDebounce->setInterval(SearchDebounceMs);
    countdown = new QTimer(this);
    countdown->setInterval(1000);

    connect(searchEdit, &QLineEdit::textChanged, searchDebounce, qOverload<>(&QTimer::start));
    connect(searchDebounce, &QTimer::timeout, this, [this]() { search(false); });
    connect(timedOutButton, &QPushButton::toggled, this, [this]() { search(false); });
    connect(pruneButton, &QPushButton::clicked, this, &GuildMembersPage::openPruneDialog);
    connect(viewNewButton, &QPushButton::clicked, this, &GuildMembersPage::viewNewMembers);
    connect(loadMoreButton, &QPushButton::clicked, this, [this]() { search(true); });
    connect(countdown, &QTimer::timeout, this, &GuildMembersPage::refreshCountdowns);
    connect(table, &QTreeWidget::customContextMenuRequested, this, &GuildMembersPage::showContextMenu);
    connect(table, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        openProfile(Core::Snowflake(item->data(NameColumn, UserIdRole).toULongLong()));
    });

    connect(images, &Core::ImageManager::imageFetched, this, &GuildMembersPage::onImageFetched);
    connect(instance, &Core::ClientInstance::ready, this, [this]() {
        if (subscribed && this->instance)
            this->instance->discord()->setMemberUpdatesSubscription(this->guildId, true);
    });
    connect(instance, &Core::ClientInstance::membersUpdated, this, &GuildMembersPage::onMembersUpdated);
    connect(instance, &Core::ClientInstance::memberRemoved, this, &GuildMembersPage::onMemberRemoved);
    connect(instance->discord(), &Discord::Client::guildMemberAdded, this, &GuildMembersPage::onMemberAdded);
    connect(instance, &Core::ClientInstance::guildRoleCreated, this, [this](const Discord::GuildRoleCreate &event) {
        if (event.guildId.get() == this->guildId)
            refreshRoles();
    });
    connect(instance, &Core::ClientInstance::guildRoleUpdated, this, [this](const Discord::GuildRoleUpdate &event) {
        if (event.guildId.get() == this->guildId)
            refreshRoles();
    });
    connect(instance, &Core::ClientInstance::guildRoleDeleted, this, [this](const Discord::GuildRoleDelete &event) {
        if (event.guildId.get() == this->guildId)
            refreshRoles();
    });
    connect(instance, &Core::ClientInstance::guildUpdated, this, [this](const Discord::Guild &guild) {
        if (guild.id.get() == this->guildId)
            pruneButton->setVisible(canPrune());
    });

    updatePermissions();
}

GuildMembersPage::~GuildMembersPage()
{
    setMemberUpdatesSubscribed(false);
}

void GuildMembersPage::load()
{
    refreshRoles();
    search(false);
}

void GuildMembersPage::updatePermissions()
{
    const bool searchable = canSearch();
    permissionNotice->setVisible(!searchable);
    pruneButton->setVisible(canPrune());
    table->setColumnHidden(JoinMethodColumn, !hasPermission(Discord::Permission::MANAGE_GUILD));
    if (isVisible())
        setMemberUpdatesSubscribed(searchable);
    if (isLoaded() && searchable != serverListing)
        search(false);
}

bool GuildMembersPage::canSearch() const
{
    return GuildSettingsAccess::canEdit(instance, guildId, GuildSettingsSection::Members);
}

void GuildMembersPage::showEvent(QShowEvent *event)
{
    setMemberUpdatesSubscribed(canSearch());
    countdown->start();
    GuildSettingsPage::showEvent(event);
}

void GuildMembersPage::hideEvent(QHideEvent *event)
{
    setMemberUpdatesSubscribed(false);
    countdown->stop();
    GuildSettingsPage::hideEvent(event);
}

void GuildMembersPage::setMemberUpdatesSubscribed(bool subscribe)
{
    if (subscribed == subscribe || !instance)
        return;
    subscribed = subscribe;
    instance->discord()->setMemberUpdatesSubscription(guildId, subscribe);
}

Discord::MemberSearchQuery GuildMembersPage::currentQuery() const
{
    Discord::MemberSearchQuery query;
    query.text = searchEdit->text().trimmed();
    query.onlyTimedOut = timedOutButton->isChecked();
    query.sort = sort;
    query.limit = PageLimit;
    return query;
}

bool GuildMembersPage::hasFilters() const
{
    const Discord::MemberSearchQuery query = currentQuery();
    return query.searchesText() || query.onlyTimedOut;
}

void GuildMembersPage::search(bool loadMore)
{
    if (!instance)
        return;
    if (!loadMore) {
        loadedAt = QDateTime::currentDateTimeUtc();
        newMemberCount = 0;
        newMembersBar->hide();
    }
    if (!canSearch()) {
        showKnownMembers(loadMore);
        return;
    }

    Discord::MemberSearchQuery query = loadMore ? listedQuery : currentQuery();
    if (loadMore) {
        if (order.isEmpty() || searching)
            return;
        const Discord::Member &last = results[order.last()].member.get();
        query.after = Discord::MemberSearchQuery::Cursor{ last.joinedAt.hasValue() ? last.joinedAt->toMSecsSinceEpoch() : 0, order.last() };
    } else {
        listedQuery = query;
        if (order.isEmpty() || !serverListing)
            showMessage(tr("Searching all members..."));
    }

    serverListing = true;
    searchGeneration++;
    searching = true;
    loadMoreButton->setEnabled(false);
    updateHeader();
    runSearch(query, loadMore, 1);
}

void GuildMembersPage::runSearch(const Discord::MemberSearchQuery &query, bool loadMore, int attempt)
{
    if (!instance)
        return;

    const int generation = searchGeneration;
    QPointer<GuildMembersPage> self(this);
    instance->discord()->searchGuildMembers(guildId, query, [self, query, loadMore, attempt, generation](const Core::Result<Discord::Client::MemberSearchPage> &result) {
        if (!self || generation != self->searchGeneration)
            return;

        auto fail = [&self, loadMore](const QString &error) {
            self->searching = false;
            if (loadMore) {
                self->loadMoreButton->setEnabled(true);
                self->showActionError(error);
            } else {
                self->applyPage({}, 0, false, false);
                self->showMessage(error);
            }
        };
        if (!result.success()) {
            fail(result.error);
            return;
        }

        if (const std::optional<int> retryAfter = result.value->indexingRetryAfterSeconds) {
            const QString indexing = tr("Before searching, we need to index this server. Give us a bit.");
            if (attempt >= MaxIndexAttempts) {
                fail(indexing);
                return;
            }
            if (self->order.isEmpty())
                self->showMessage(indexing);
            QTimer::singleShot(*retryAfter * 1000, self.data(), [self, query, loadMore, attempt, generation]() {
                if (self && generation == self->searchGeneration)
                    self->runSearch(query, loadMore, attempt + 1);
            });
            return;
        }

        self->searching = false;
        QList<Discord::User> users;
        QList<Discord::Member> members;
        for (const auto &found : result.value->members) {
            if (!userIdOf(found).isValid())
                continue;
            users.append(found.member->user.get());
            members.append(found.member.get());
        }
        if (self->instance) {
            self->instance->users()->saveUsers(users);
            self->instance->users()->saveMembers(self->guildId, members);
        }
        self->applyPage(result.value->members, result.value->totalResultCount, loadMore, result.value->members.size() >= query.limit);
    });
}

void GuildMembersPage::showKnownMembers(bool loadMore)
{
    searchGeneration++;
    searching = false;
    serverListing = false;
    if (!loadMore)
        knownMatches = matchKnownMembers(currentQuery());
    const int from = loadMore ? int(order.size()) : 0;
    const QList<Discord::MemberSearchResult> page = knownMatches.mid(from, PageLimit);
    applyPage(page, int(knownMatches.size()), loadMore, from + page.size() < knownMatches.size());
}

QList<Discord::MemberSearchResult> GuildMembersPage::matchKnownMembers(const Discord::MemberSearchQuery &query) const
{
    QList<Discord::MemberSearchResult> matches;
    if (!instance)
        return matches;

    QStringList terms;
    if (query.searchesText())
        for (const QString &term : query.text.split(QLatin1Char(','), Qt::SkipEmptyParts))
            if (!term.trimmed().isEmpty())
                terms.append(term.trimmed());
    const QDateTime now = QDateTime::currentDateTimeUtc();

    for (const Discord::Member &member : instance->users()->getKnownMembers(guildId)) {
        if (!member.user.hasValue())
            continue;
        const Core::Snowflake userId = member.user->id.get();
        if (query.onlyTimedOut && !(member.communicationDisabledUntil.hasValue() && member.communicationDisabledUntil.get() > now))
            continue;

        const Discord::User &user = member.user.get();
        const bool matched = terms.isEmpty() || std::any_of(terms.cbegin(), terms.cend(), [&](const QString &term) {
                                 return term == QString::number(quint64(userId)) ||
                                        user.username->contains(term, Qt::CaseInsensitive) ||
                                        (user.globalName.hasValue() && user.globalName->contains(term, Qt::CaseInsensitive)) ||
                                        (member.nick.hasValue() && member.nick->contains(term, Qt::CaseInsensitive));
                             });
        if (!matched)
            continue;
        Discord::MemberSearchResult result;
        result.member = member;
        matches.append(result);
    }

    using Sort = Discord::MemberSearchQuery::Sort;
    const Sort sortOrder = query.sort.value_or(Sort::JoinedNewest);
    auto joinedAt = [](const Discord::MemberSearchResult &result) {
        const Discord::Member &member = result.member.get();
        return member.joinedAt.hasValue() ? member.joinedAt->toMSecsSinceEpoch() : 0;
    };
    std::sort(matches.begin(), matches.end(), [&](const Discord::MemberSearchResult &a, const Discord::MemberSearchResult &b) {
        switch (sortOrder) {
        case Sort::JoinedNewest:
            return joinedAt(a) > joinedAt(b);
        case Sort::JoinedOldest:
            return joinedAt(a) < joinedAt(b);
        case Sort::UserIdDescending:
            return quint64(userIdOf(a)) > quint64(userIdOf(b));
        case Sort::UserIdAscending:
            return quint64(userIdOf(a)) < quint64(userIdOf(b));
        }
        return false;
    });
    return matches;
}

void GuildMembersPage::applyPage(const QList<Discord::MemberSearchResult> &page, int total, bool loadMore, bool mayHaveMore)
{
    if (!loadMore) {
        order.clear();
        results.clear();
    }

    const int firstNew = int(order.size());
    for (const auto &result : page) {
        const Core::Snowflake userId = userIdOf(result);
        if (!userId.isValid())
            continue;
        if (!results.contains(userId))
            order.append(userId);
        results.insert(userId, result);
    }

    totalResults = qMax(total, int(order.size()));
    loadMoreButton->setVisible(mayHaveMore && order.size() < totalResults);
    loadMoreButton->setEnabled(true);
    if (loadMore)
        appendRows(firstNew);
    else
        rebuildTable();
}

void GuildMembersPage::rebuildTable()
{
    table->clear();
    rows.clear();
    avatarTracker.clear();
    appendRows(0);

    if (order.isEmpty() && !searching)
        showMessage(hasFilters() ? tr("No members match these search results.") : QString());
    else if (!order.isEmpty())
        states->setCurrentWidget(table);
    updateHeader();
}

void GuildMembersPage::appendRows(int from)
{
    for (int index = from; index < order.size(); index++) {
        const Core::Snowflake userId = order[index];
        auto *item = new QTreeWidgetItem(table);
        rows.insert(userId, item);
        fillRow(item, results[userId]);
    }
    updateFooter();
}

void GuildMembersPage::fillRow(QTreeWidgetItem *item, const Discord::MemberSearchResult &result)
{
    if (!instance)
        return;

    const Discord::Member &member = result.member.get();
    const Core::Snowflake userId = member.user->id.get();
    const auto knownUser = instance->users()->getUser(userId);
    const Discord::User &user = knownUser ? *knownUser : member.user.get();
    const QList<Core::Snowflake> roleIds = member.roles.valueOr({});

    item->setData(NameColumn, UserIdRole, QVariant::fromValue<quint64>(userId));
    item->setText(NameColumn, instance->users()->getDisplayName(userId, guildId));
    item->setData(NameColumn, TagRole, user.tag());
    item->setData(NameColumn, NameColorRole, nameColor(roleIds));

    if (member.joinedAt.hasValue() && member.joinedAt->isValid()) {
        item->setText(MemberSinceColumn, memberSinceText(member.joinedAt.get()));
        item->setToolTip(MemberSinceColumn, fullDate(member.joinedAt.get()));
    }
    const QDateTime createdAt = userId.toDateTime();
    item->setText(JoinedDiscordColumn, accountAgeText(createdAt));
    item->setToolTip(JoinedDiscordColumn, fullDate(createdAt));

    QString joinTooltip;
    item->setText(JoinMethodColumn, joinMethodText(result, &joinTooltip));
    item->setToolTip(JoinMethodColumn, joinTooltip);

    QList<Discord::Role> held;
    for (const Discord::Role &role : sortedRoles)
        if (roleIds.contains(role.id.get()))
            held.append(role);
    if (held.isEmpty()) {
        item->setText(RolesColumn, QString());
        item->setIcon(RolesColumn, QIcon());
        item->setToolTip(RolesColumn, QString());
    } else {
        const Discord::Role &highest = held.first();
        item->setText(RolesColumn, held.size() > 1 ? QStringLiteral("%1 +%2").arg(highest.name.get()).arg(held.size() - 1) : highest.name.get());
        item->setIcon(RolesColumn, colorDot(highest.hasColor() ? highest.getColor() : palette().color(QPalette::PlaceholderText)));
        QStringList names;
        for (const Discord::Role &role : held)
            names.append(role.name.get());
        item->setToolTip(RolesColumn, names.join(QStringLiteral(", ")));
    }

    QString signalsTooltip;
    item->setText(SignalsColumn, signalsText(member, &signalsTooltip));
    item->setToolTip(SignalsColumn, signalsTooltip);

    const QUrl avatarUrl = instance->users()->getAvatarUrl(user, guildId, 64);
    const QPixmap avatar = avatarTracker.fetch(images, avatarUrl, AvatarSize, userId, instance->accountId());
    item->setIcon(NameColumn, QIcon(roundedPixmap(avatar, AvatarSize.width(), AvatarSize.width() / 2.0)));
}

QColor GuildMembersPage::nameColor(const QList<Core::Snowflake> &roleIds) const
{
    for (const Discord::Role &role : sortedRoles)
        if (role.hasColor() && roleIds.contains(role.id.get()))
            return role.getColor();
    return {};
}

QString GuildMembersPage::signalsText(const Discord::Member &member, QString *tooltip) const
{
    QStringList parts;
    QStringList tips;
    if (member.communicationDisabledUntil.hasValue() && member.communicationDisabledUntil.get() > QDateTime::currentDateTimeUtc()) {
        parts.append(tr("Timed out for %1").arg(MemberModeration::timeoutRemaining(member.communicationDisabledUntil.get())));
        tips.append(fullDate(member.communicationDisabledUntil.get()));
    }
    if (member.unusualDmActivityUntil.hasValue() && member.unusualDmActivityUntil.get() >= loadedAt.addSecs(-UnusualDmWindowSecs)) {
        parts.append(tr("Unusual DM Activity"));
        tips.append(tr("Sent excessive DMs to non-friend server members in last 24 hrs"));
    }
    if (member.flags.valueOr(0) & Discord::Member::FlagQuarantinedName) {
        parts.append(tr("Quarantined"));
        tips.append(tr("User can't talk in server until they change their member name"));
    }
    *tooltip = tips.join(QLatin1Char('\n'));
    return parts.join(QStringLiteral(", "));
}

QString GuildMembersPage::joinMethodText(const Discord::MemberSearchResult &result, QString *tooltip) const
{
    const QString code = result.sourceInviteCode.valueOr(QString());
    switch (result.resolvedJoinSource()) {
    case Discord::JoinSourceType::BOT:
        *tooltip = tr("Added by Bot");
        return tr("Bot Invite");
    case Discord::JoinSourceType::INTEGRATION:
        return tr("Integration");
    case Discord::JoinSourceType::DISCOVERY:
        return tr("Server Discovery");
    case Discord::JoinSourceType::HUB:
        return tr("Student Hub");
    case Discord::JoinSourceType::INVITE:
        if (instance && result.inviterId.hasValue()) {
            if (const auto inviter = instance->users()->getUser(result.inviterId.get()))
                *tooltip = tr("Invited by %1").arg(inviter->tag());
        }
        return code;
    case Discord::JoinSourceType::VANITY_URL:
        *tooltip = tr("Vanity URL");
        return code.isEmpty() ? tr("Vanity URL") : code;
    case Discord::JoinSourceType::MANUAL_MEMBER_VERIFICATION:
        return code.isEmpty() ? tr("Manual Verification") : tr("Manual Verification (%1)").arg(code);
    case Discord::JoinSourceType::LINKED_CHANNEL:
        return tr("Linked Channel");
    case Discord::JoinSourceType::UNSPECIFIED:
        break;
    }
    *tooltip = tr("Join method not available");
    return tr("Unknown");
}

MemberModerationTarget GuildMembersPage::targetOf(const Discord::MemberSearchResult &result) const
{
    const Discord::Member &member = result.member.get();
    MemberModerationTarget target;
    target.guildId = guildId;
    target.userId = member.user->id.get();
    target.username = member.user->username.get();
    target.roleIds = member.roles.valueOr({});
    target.bot = member.user->bot.valueOr(false);
    if (member.communicationDisabledUntil.hasValue())
        target.timeoutUntil = member.communicationDisabledUntil.get();
    return target;
}

void GuildMembersPage::refreshCountdowns()
{
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        const auto result = results.constFind(it.key());
        if (result == results.constEnd() || !result->member->communicationDisabledUntil.hasValue())
            continue;
        QString tooltip;
        it.value()->setText(SignalsColumn, signalsText(result->member.get(), &tooltip));
        it.value()->setToolTip(SignalsColumn, tooltip);
    }
}

void GuildMembersPage::refreshRoles()
{
    if (!instance)
        return;
    sortedRoles = Core::RoleHierarchy::sorted(instance->getRolesForGuild(guildId), guildId);
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it)
        fillRow(it.value(), results[it.key()]);
}

void GuildMembersPage::updateHeader()
{
    sectionTitle->setText(hasFilters() ? tr("Search Results") : tr("Recent Members"));
}

void GuildMembersPage::updateFooter()
{
    const int shown = order.size();
    if (shown == 0) {
        footerLabel->clear();
        return;
    }
    const QString showing = countText(shown, tr("Showing <b>1</b> member"), tr("Showing <b>%1</b> members"));
    footerLabel->setText(totalResults > shown ? tr("%1 of <b>%2</b>").arg(showing).arg(totalResults) : showing);
}

void GuildMembersPage::showMessage(const QString &text)
{
    messageLabel->setText(text);
    states->setCurrentWidget(messageLabel);
}

bool GuildMembersPage::canPrune() const
{
    if (!instance)
        return false;
    const auto guild = instance->getGuild(guildId);
    if (!guild)
        return false;
    if (guild->hasFeature(QStringLiteral("PRUNE_REQUIRES_ADMIN")))
        return guild->ownerId.get() == selfId() || hasPermission(Discord::Permission::ADMINISTRATOR);
    return hasPermission(Discord::Permission::MANAGE_GUILD | Discord::Permission::KICK_MEMBERS);
}

void GuildMembersPage::onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap)
{
    if (size != AvatarSize)
        return;
    const QIcon icon(roundedPixmap(pixmap, AvatarSize.width(), AvatarSize.width() / 2.0));
    avatarTracker.notify(url, [this, &icon](Core::Snowflake userId) {
        if (QTreeWidgetItem *item = rows.value(userId))
            item->setIcon(NameColumn, icon);
    });
}

void GuildMembersPage::onMembersUpdated(Core::Snowflake changedGuildId, const QList<Core::Snowflake> &userIds)
{
    if (changedGuildId != guildId || !instance)
        return;

    for (Core::Snowflake userId : userIds) {
        auto row = rows.constFind(userId);
        auto result = results.find(userId);
        if (row == rows.constEnd() || result == results.end())
            continue;
        const auto member = instance->users()->getMember(guildId, userId);
        if (!member)
            continue;

        Discord::Member updated = *member;
        const Discord::Member &previous = result->member.get();
        if (const auto user = instance->users()->getUser(userId))
            updated.user = *user;
        else if (!updated.user.hasValue())
            updated.user = previous.user;
        if (updated.unusualDmActivityUntil.isUndefined())
            updated.unusualDmActivityUntil = previous.unusualDmActivityUntil;
        result->member = updated;
        fillRow(row.value(), result.value());
    }
}

void GuildMembersPage::onMemberRemoved(Core::Snowflake changedGuildId, Core::Snowflake userId)
{
    if (changedGuildId != guildId || !results.contains(userId))
        return;
    results.remove(userId);
    order.removeAll(userId);
    delete rows.take(userId);
    totalResults = qMax(0, totalResults - 1);
    if (order.isEmpty())
        rebuildTable();
    else
        updateFooter();
}

void GuildMembersPage::onMemberAdded(const Discord::GuildMemberAdd &event)
{
    if (event.guildId.get() != guildId || !loadedAt.isValid())
        return;
    newMemberCount++;
    newMembersLabel->setText(countText(newMemberCount, tr("1 new member since %2"), tr("%1 new members since %2"))
                                     .arg(QLocale::system().toString(loadedAt.toLocalTime().time(), QLocale::ShortFormat)));
    newMembersBar->show();
}

void GuildMembersPage::viewNewMembers()
{
    auto resetSearch = [this]() {
        {
            QSignalBlocker searchBlocker(searchEdit);
            QSignalBlocker timedOutBlocker(timedOutButton);
            searchEdit->clear();
            timedOutButton->setChecked(false);
        }
        searchDebounce->stop();
        search(false);
    };
    if (!hasFilters()) {
        resetSearch();
        return;
    }

    auto *confirm = new ConfirmPopup(tr("View New Members?"), tr("Doing this will reset your search. Are you sure you want to continue?"), tr("Confirm"), this);
    confirm->setAttribute(Qt::WA_DeleteOnClose);
    connect(confirm, &QDialog::accepted, this, resetSearch);
    confirm->open();
}

void GuildMembersPage::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = table->itemAt(pos);
    if (!item || !instance)
        return;
    const Core::Snowflake userId(item->data(NameColumn, UserIdRole).toULongLong());
    const auto result = results.constFind(userId);
    if (result == results.constEnd())
        return;

    QMenu menu(this);
    menu.addAction(tr("Profile"), this, [this, userId]() { openProfile(userId); });
    menu.addSeparator();
    MemberModeration::addModerationActions(&menu, this, instance, targetOf(result.value()));
    menu.addSeparator();
    menu.addAction(tr("Copy User ID"), this, [userId]() { QGuiApplication::clipboard()->setText(QString::number(quint64(userId))); });
    menu.exec(table->viewport()->mapToGlobal(pos));
}

void GuildMembersPage::openProfile(Core::Snowflake userId)
{
    if (!instance)
        return;
    auto *popup = new UserProfilePopup(images, instance, userId, guildId, this);
    connect(popup, &UserProfilePopup::linkActivated, this, &GuildMembersPage::linkActivated);
    popup->show();
}

void GuildMembersPage::openPruneDialog()
{
    if (!instance || !canPrune())
        return;
    const auto guild = instance->getGuild(guildId);
    const auto hierarchy = instance->selfRoleHierarchy(guildId);
    if (!guild || !hierarchy)
        return;

    QList<Discord::Role> includable;
    for (const Discord::Role &role : hierarchy->sortedRoles())
        if (role.id.get() != guildId && hierarchy->outranksRole(role))
            includable.append(role);

    auto *dialog = new PruneDialog(instance, guildId, guild->name.get(), includable, this);
    dialog->open();
}

} // namespace UI
} // namespace Acheron
