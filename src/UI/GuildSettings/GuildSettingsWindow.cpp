#include "GuildSettingsWindow.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "Core/ClientInstance.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Theme/Icons.hpp"
#include "Core/Theme/Manager.hpp"
#include "Discord/CdnUrls.hpp"
#include "UI/Dialogs/BasePopup.hpp"
#include "UI/Dialogs/ConfirmPopup.hpp"

#include "GuildAuditLogPage.hpp"
#include "GuildBansPage.hpp"
#include "GuildBoostPerksPage.hpp"
#include "GuildEmojiPage.hpp"
#include "GuildEngagementPage.hpp"
#include "GuildInvitesPage.hpp"
#include "GuildMembersPage.hpp"
#include "GuildProfilePage.hpp"
#include "GuildRolesPage.hpp"
#include "GuildStickersPage.hpp"

namespace Acheron {
namespace UI {

namespace {

constexpr int SectionRole = Qt::UserRole;
constexpr QSize IconSize(64, 64);

std::optional<GuildSettingsSection> sectionOf(const QListWidgetItem *item)
{
    const QVariant value = item ? item->data(SectionRole) : QVariant();
    if (!value.isValid())
        return std::nullopt;
    return static_cast<GuildSettingsSection>(value.toInt());
}

class DeleteServerDialog : public BasePopup
{
public:
    DeleteServerDialog(const QString &guildName, bool requireName, QWidget *parent)
        : BasePopup(parent), guildName(guildName)
    {
        auto *layout = new QVBoxLayout(getContainer());
        layout->setSpacing(12);
        layout->setContentsMargins(24, 24, 24, 24);

        auto *title = new QLabel(tr("Delete '%1'").arg(guildName), getContainer());
        QFont titleFont = title->font();
        titleFont.setBold(true);
        titleFont.setPointSize(titleFont.pointSize() + 2);
        title->setFont(titleFont);
        layout->addWidget(title);

        auto *body = new QLabel(tr("Are you sure you want to delete <b>%1</b>? This action cannot be undone.").arg(guildName.toHtmlEscaped()), getContainer());
        body->setWordWrap(true);
        layout->addWidget(body);

        if (requireName) {
            layout->addWidget(new QLabel(tr("Enter server name"), getContainer()));
            nameEdit = new QLineEdit(getContainer());
            layout->addWidget(nameEdit);

            errorLabel = new QLabel(tr("You didn't enter the server name correctly"), getContainer());
            errorLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Core::Theme::Manager::instance().color(Core::Theme::Token::ChatError).name()));
            errorLabel->hide();
            layout->addWidget(errorLabel);
        }

        auto *buttons = new QDialogButtonBox(getContainer());
        QPushButton *deleteButton = buttons->addButton(tr("Delete Server"), QDialogButtonBox::AcceptRole);
        buttons->addButton(QDialogButtonBox::Cancel);
        deleteButton->setDefault(true);
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            if (nameEdit && nameEdit->text().trimmed().compare(this->guildName.trimmed(), Qt::CaseInsensitive) != 0) {
                errorLabel->show();
                return;
            }
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
    }

private:
    QString guildName;
    QLineEdit *nameEdit = nullptr;
    QLabel *errorLabel = nullptr;
};

} // namespace

GuildSettingsWindow::GuildSettingsWindow(Core::ImageManager *images, Core::ClientInstance *instance,
                                         Core::Snowflake guildId, QWidget *parent)
    : QWidget(parent, Qt::Window), instance(instance), images(images), guildId(guildId)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_QuitOnClose, false);
    resize(980, 700);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    navigation = new QListWidget(this);
    navigation->setFixedWidth(210);
    navigation->setFrameShape(QFrame::NoFrame);
    navigation->setSpacing(1);
    layout->addWidget(navigation);

    stack = new QStackedWidget(this);
    layout->addWidget(stack, 1);

    connect(navigation, &QListWidget::currentItemChanged, this, &GuildSettingsWindow::onNavigationChanged);

    connect(instance, &Core::ClientInstance::stateChanged, this, [this](Core::ConnectionState state) {
        if (state == Core::ConnectionState::Disconnected)
            forceClose();
    });
    connect(instance, &Core::ClientInstance::guildRemoved, this, [this](Core::Snowflake removedGuildId) {
        if (removedGuildId == this->guildId)
            forceClose();
    });
    connect(instance, &Core::ClientInstance::guildUpdated, this, [this](const Discord::Guild &guild) {
        if (guild.id.get() == this->guildId) {
            updateTitle();
            rebuildNavigation();
        }
    });
    connect(instance->permissions(), &Core::PermissionManager::guildPermissionsChanged, this, &GuildSettingsWindow::onGuildPermissionsChanged);
    connect(images, &Core::ImageManager::imageFetched, this, [this](const QUrl &url, const QSize &size, const QPixmap &pixmap) {
        if (size == IconSize && url == iconUrl)
            setWindowIcon(QIcon(pixmap));
    });

    shownPermissions = instance->permissions()->getGuildPermissions(instance->accountId(), guildId);
    rebuildNavigation();
    updateTitle();
}

void GuildSettingsWindow::openSection(GuildSettingsSection section)
{
    QListWidgetItem *item = itemFor(section);
    if (!item || section == GuildSettingsSection::DeleteServer)
        item = itemFor(GuildSettingsSection::Profile);
    if (item)
        navigation->setCurrentItem(item);
}

void GuildSettingsWindow::rebuildNavigation()
{
    if (!instance)
        return;

    const QList<GuildSettingsSection> sections = GuildSettingsAccess::visibleSections(instance, guildId);
    const std::optional<GuildSettingsSection> current = sectionOf(navigation->currentItem());
    if (sections.isEmpty()) {
        forceClose();
        return;
    }

    {
        QSignalBlocker blocker(navigation);
        fillNavigation(sections);
        if (current && sections.contains(*current))
            navigation->setCurrentItem(itemFor(*current));
    }
    if (current && !sections.contains(*current))
        openSection(GuildSettingsSection::Profile);
}

void GuildSettingsWindow::fillNavigation(const QList<GuildSettingsSection> &sections)
{
    const auto guild = instance->getGuild(guildId);
    const QColor mutedColor = palette().color(QPalette::PlaceholderText);
    const QIcon lockIcon = Core::Theme::Icons::icon(Core::Theme::Icons::Name::Lock, mutedColor);
    QPixmap blank(64, 64);
    blank.fill(Qt::transparent);
    const QIcon blankIcon(blank);

    navigation->clear();
    std::optional<GuildSettingsGroup> lastGroup;
    for (GuildSettingsSection section : sections) {
        const GuildSettingsGroup group = GuildSettingsAccess::group(section);
        if (group != lastGroup) {
            lastGroup = group;
            QString header = group == GuildSettingsGroup::Server && guild ? guild->name.get() : GuildSettingsAccess::groupTitle(group);
            if (group == GuildSettingsGroup::Server && header.isEmpty())
                header = tr("Server Settings");
            auto *headerItem = new QListWidgetItem(header, navigation);
            headerItem->setFlags(Qt::NoItemFlags);
            QFont font = navigation->font();
            font.setBold(true);
            font.setPointSizeF(font.pointSizeF() * 0.9);
            headerItem->setFont(font);
            headerItem->setForeground(mutedColor);
            headerItem->setTextAlignment(Qt::AlignLeft | Qt::AlignBottom);
            const int gapAbove = navigation->count() > 1 ? 16 : 8;
            headerItem->setSizeHint(QSize(0, QFontMetrics(font).height() + gapAbove));
        }

        auto *item = new QListWidgetItem(GuildSettingsAccess::title(section), navigation);
        item->setData(SectionRole, static_cast<int>(section));
        const bool editable = GuildSettingsAccess::canEdit(instance, guildId, section);
        if (section == GuildSettingsSection::DeleteServer) {
            const QColor color = editable ? Core::Theme::Manager::instance().color(Core::Theme::Token::ChatError) : mutedColor;
            item->setForeground(color);
            item->setIcon(Core::Theme::Icons::icon(Core::Theme::Icons::Name::Trash, color));
            if (!editable) {
                item->setFlags(Qt::NoItemFlags);
                item->setToolTip(tr("This server requires two-factor authentication to delete it"));
            }
            continue;
        }
        item->setIcon(editable ? blankIcon : lockIcon);
        if (!editable)
            item->setToolTip(tr("Read only: you don't have permission to change anything here"));
    }
}

void GuildSettingsWindow::updateTitle()
{
    const auto guild = instance ? instance->getGuild(guildId) : std::nullopt;
    if (!guild)
        return;

    setWindowTitle(tr("%1 — Server Settings").arg(guild->name.get()));

    iconUrl = Discord::Cdn::guildIcon(guildId, guild->icon.get(), IconSize.width());
    if (iconUrl.isEmpty()) {
        setWindowIcon(QIcon());
        return;
    }
    if (images->isCached(iconUrl, IconSize))
        setWindowIcon(QIcon(images->get(iconUrl, IconSize, instance->accountId())));
    else
        images->get(iconUrl, IconSize, instance->accountId());
}

void GuildSettingsWindow::onNavigationChanged(QListWidgetItem *current, QListWidgetItem *previous)
{
    const std::optional<GuildSettingsSection> section = sectionOf(current);
    if (!section)
        return;

    if (*section == GuildSettingsSection::DeleteServer) {
        QSignalBlocker blocker(navigation);
        navigation->setCurrentItem(previous);
        confirmDeleteServer();
        return;
    }

    GuildSettingsPage *page = currentPage();
    if (page && page->hasUnsavedChanges()) {
        QSignalBlocker blocker(navigation);
        navigation->setCurrentItem(previous);
        page->warnUnsavedChanges();
        return;
    }

    GuildSettingsPage *next = pageFor(*section);
    stack->setCurrentWidget(next);
    next->activate();
}

void GuildSettingsWindow::onGuildPermissionsChanged(Core::Snowflake changedGuildId)
{
    if (changedGuildId != guildId || !instance)
        return;
    const Discord::Permissions current = instance->permissions()->getGuildPermissions(instance->accountId(), guildId);
    if (current == shownPermissions)
        return;
    shownPermissions = current;
    rebuildNavigation();
}

void GuildSettingsWindow::forceClose()
{
    forcedClose = true;
    auto isOurs = [this](const QObject *object) {
        for (; object; object = object->parent())
            if (object == this)
                return true;
        return false;
    };
    while (QWidget *popup = QApplication::activePopupWidget()) {
        if (!isOurs(popup))
            break;
        popup->close();
        if (QApplication::activePopupWidget() == popup)
            break;
    }
    close();
}

void GuildSettingsWindow::confirmDeleteServer()
{
    if (!instance)
        return;
    const auto guild = instance->getGuild(guildId);
    if (!guild)
        return;

    auto *dialog = new DeleteServerDialog(guild->name.get(), !GuildSettingsAccess::selfHasMfa(instance), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this]() {
        if (!instance)
            return;
        QPointer<GuildSettingsWindow> self(this);
        instance->discord()->deleteGuild(guildId, [self](const Core::Result<void> &result) {
            if (self && !result.success())
                GuildSettingsPage::showError(self, result.error);
        });
    });
    dialog->open();
}

GuildSettingsPage *GuildSettingsWindow::pageFor(GuildSettingsSection section)
{
    auto existing = pages.constFind(section);
    if (existing != pages.constEnd())
        return existing.value();

    GuildSettingsPage *page = nullptr;
    switch (section) {
    case GuildSettingsSection::Profile:
        page = new GuildProfilePage(instance, images, guildId, stack);
        break;
    case GuildSettingsSection::Engagement:
        page = new GuildEngagementPage(instance, images, guildId, stack);
        break;
    case GuildSettingsSection::BoostPerks:
        page = new GuildBoostPerksPage(instance, images, guildId, stack);
        break;
    case GuildSettingsSection::Emoji:
        page = new GuildEmojiPage(instance, images, guildId, stack);
        break;
    case GuildSettingsSection::Stickers:
        page = new GuildStickersPage(instance, images, guildId, stack);
        break;
    case GuildSettingsSection::Members: {
        auto *members = new GuildMembersPage(instance, images, guildId, stack);
        connect(members, &GuildMembersPage::linkActivated, this, &GuildSettingsWindow::linkActivated);
        page = members;
        break;
    }
    case GuildSettingsSection::Roles:
        page = new GuildRolesPage(instance, images, guildId, stack);
        break;
    case GuildSettingsSection::Invites:
        page = new GuildInvitesPage(instance, images, guildId, stack);
        break;
    case GuildSettingsSection::AuditLog:
        page = new GuildAuditLogPage(instance, images, guildId, stack);
        break;
    case GuildSettingsSection::Bans:
        page = new GuildBansPage(instance, images, guildId, stack);
        break;
    case GuildSettingsSection::DeleteServer:
        return nullptr;
    }

    stack->addWidget(page);
    pages.insert(section, page);
    return page;
}

GuildSettingsPage *GuildSettingsWindow::currentPage() const
{
    return qobject_cast<GuildSettingsPage *>(stack->currentWidget());
}

QListWidgetItem *GuildSettingsWindow::itemFor(GuildSettingsSection section) const
{
    for (int row = 0; row < navigation->count(); row++) {
        QListWidgetItem *item = navigation->item(row);
        if (sectionOf(item) == section)
            return item;
    }
    return nullptr;
}

void GuildSettingsWindow::closeEvent(QCloseEvent *event)
{
    GuildSettingsPage *page = currentPage();
    if (forcedClose || !page || !page->hasUnsavedChanges()) {
        QWidget::closeEvent(event);
        return;
    }

    event->ignore();
    if (discardConfirmation)
        return;
    auto *confirm = new ConfirmPopup(tr("Discard unsaved changes?"),
                                     tr("You have unsaved changes, are you sure you want to discard them?"), tr("Discard"),
                                     this);
    confirm->setAttribute(Qt::WA_DeleteOnClose);
    discardConfirmation = confirm;
    connect(confirm, &QDialog::accepted, this, &GuildSettingsWindow::forceClose);
    connect(confirm, &QDialog::rejected, this, [this]() {
        if (GuildSettingsPage *current = currentPage())
            current->warnUnsavedChanges();
    });
    confirm->open();
}

} // namespace UI
} // namespace Acheron
