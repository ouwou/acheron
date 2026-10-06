#pragma once

#include <QDateTime>
#include <QHash>
#include <QUrl>

#include <optional>

#include "Discord/Entities.hpp"
#include "Discord/GuildRequests.hpp"
#include "GuildSettingsPage.hpp"
#include "UI/AvatarRequestTracker.hpp"

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace Acheron {
namespace Discord {
struct GuildMemberAdd;
} // namespace Discord
namespace UI {

struct MemberModerationTarget;

class GuildMembersPage : public GuildSettingsPage
{
    Q_OBJECT
public:
    GuildMembersPage(Core::ClientInstance *instance, Core::ImageManager *images, Core::Snowflake guildId, QWidget *parent = nullptr);
    ~GuildMembersPage() override;

signals:
    void linkActivated(const QString &url);

protected:
    void load() override;
    void updatePermissions() override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    [[nodiscard]] bool canSearch() const;
    void search(bool loadMore);
    void runSearch(const Discord::MemberSearchQuery &query, bool loadMore, int attempt);
    void showKnownMembers(bool loadMore);
    [[nodiscard]] QList<Discord::MemberSearchResult> matchKnownMembers(const Discord::MemberSearchQuery &query) const;
    void applyPage(const QList<Discord::MemberSearchResult> &page, int total, bool loadMore, bool mayHaveMore);
    void rebuildTable();
    void appendRows(int from);
    void fillRow(QTreeWidgetItem *item, const Discord::MemberSearchResult &result);
    void refreshCountdowns();
    void refreshRoles();
    void updateHeader();
    void updateFooter();
    void showMessage(const QString &text);
    void setMemberUpdatesSubscribed(bool subscribed);

    void onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap);
    void onMembersUpdated(Core::Snowflake changedGuildId, const QList<Core::Snowflake> &userIds);
    void onMemberRemoved(Core::Snowflake changedGuildId, Core::Snowflake userId);
    void onMemberAdded(const Discord::GuildMemberAdd &event);

    void showContextMenu(const QPoint &pos);
    void openProfile(Core::Snowflake userId);
    void viewNewMembers();
    void openPruneDialog();

    [[nodiscard]] Discord::MemberSearchQuery currentQuery() const;
    [[nodiscard]] bool hasFilters() const;
    [[nodiscard]] bool canPrune() const;
    [[nodiscard]] QColor nameColor(const QList<Core::Snowflake> &roleIds) const;
    [[nodiscard]] QString signalsText(const Discord::Member &member, QString *tooltip) const;
    [[nodiscard]] QString joinMethodText(const Discord::MemberSearchResult &result, QString *tooltip) const;
    [[nodiscard]] MemberModerationTarget targetOf(const Discord::MemberSearchResult &result) const;

    QWidget *permissionNotice;
    QLabel *sectionTitle;
    QLineEdit *searchEdit;
    QPushButton *sortButton;
    QPushButton *timedOutButton;
    QPushButton *pruneButton;
    QWidget *newMembersBar;
    QLabel *newMembersLabel;
    QStackedWidget *states;
    QTreeWidget *table;
    QLabel *messageLabel;
    QLabel *footerLabel;
    QPushButton *loadMoreButton;
    QTimer *searchDebounce;
    QTimer *countdown;

    std::optional<Discord::MemberSearchQuery::Sort> sort;
    Discord::MemberSearchQuery listedQuery;
    QList<Core::Snowflake> order;
    QHash<Core::Snowflake, Discord::MemberSearchResult> results;
    QHash<Core::Snowflake, QTreeWidgetItem *> rows;
    AvatarRequestTracker<Core::Snowflake> avatarTracker;
    QList<Discord::Role> sortedRoles;
    QList<Discord::MemberSearchResult> knownMatches;
    int totalResults = 0;
    int searchGeneration = 0;
    bool searching = false;
    bool serverListing = false;
    bool subscribed = false;
    QDateTime loadedAt;
    int newMemberCount = 0;
};

} // namespace UI
} // namespace Acheron
