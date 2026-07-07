#include "UserProfilePopup.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

#include "Core/ClientInstance.hpp"
#include "Core/ImageManager.hpp"
#include "Core/RelationshipManager.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"

namespace Acheron {
namespace UI {

namespace {

constexpr int PopupWidth = 640;
constexpr int BannerHeight = 96;
constexpr int AvatarSize = 88;
constexpr int AvatarOverlap = 60;
constexpr int ContentSidePadding = 16;
constexpr int ColumnSpacing = 20;
constexpr int IdentityGap = 12;
constexpr int LeftRightColumnHeight = 260;

class CroppedImageLabel : public QLabel
{
public:
    using QLabel::QLabel;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window));
        const QPixmap pm = pixmap(Qt::ReturnByValue);
        if (pm.isNull())
            return;
        const QPixmap scaled = pm.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const int dx = (scaled.width() - width()) / 2;
        const int dy = (scaled.height() - height()) / 2;
        p.drawPixmap(-dx, -dy, scaled);
    }
};

QString formatDate(const QDateTime &dt)
{
    if (!dt.isValid())
        return QStringLiteral("—");
    return QLocale::system().toString(dt.toLocalTime().date(), QStringLiteral("MMM d, yyyy"));
}

QString sectionHeading(const QString &text, const QColor &color)
{
    return QStringLiteral(
                   "<span style=\"color:%1; font-size:10px; font-weight:600; "
                   "letter-spacing:0.5px;\">%2</span>")
            .arg(color.name(), text.toUpper().toHtmlEscaped());
}

QString prettifyConnectionType(const QString &type)
{
    if (type == QStringLiteral("github"))
        return QStringLiteral("GitHub");
    if (type == QStringLiteral("twitter"))
        return QStringLiteral("X");
    if (type == QStringLiteral("xbox"))
        return QStringLiteral("Xbox");
    if (type == QStringLiteral("playstation"))
        return QStringLiteral("PlayStation");
    if (type == QStringLiteral("riotgames"))
        return QStringLiteral("Riot Games");
    if (type == QStringLiteral("battlenet"))
        return QStringLiteral("Battle.net");
    if (type == QStringLiteral("epicgames"))
        return QStringLiteral("Epic Games");
    if (type == QStringLiteral("leagueoflegends"))
        return QStringLiteral("League of Legends");
    if (type == QStringLiteral("ebay"))
        return QStringLiteral("eBay");
    if (type == QStringLiteral("youtube"))
        return QStringLiteral("YouTube");
    if (type == QStringLiteral("tiktok"))
        return QStringLiteral("TikTok");
    if (type == QStringLiteral("paypal"))
        return QStringLiteral("PayPal");
    QString pretty = type;
    if (!pretty.isEmpty())
        pretty[0] = pretty[0].toUpper();
    return pretty;
}

QUrl buildConnectionUrl(const QString &type, const QString &id, const QString &name)
{
    if (type == QStringLiteral("github"))
        return QUrl(QStringLiteral("https://github.com/%1").arg(name));
    if (type == QStringLiteral("twitter") || type == QStringLiteral("x"))
        return QUrl(QStringLiteral("https://twitter.com/%1").arg(name));
    if (type == QStringLiteral("spotify"))
        return QUrl(QStringLiteral("https://open.spotify.com/user/%1").arg(id));
    if (type == QStringLiteral("youtube"))
        return QUrl(QStringLiteral("https://www.youtube.com/channel/%1").arg(id));
    if (type == QStringLiteral("twitch"))
        return QUrl(QStringLiteral("https://www.twitch.tv/%1").arg(name));
    if (type == QStringLiteral("reddit"))
        return QUrl(QStringLiteral("https://www.reddit.com/u/%1").arg(name));
    if (type == QStringLiteral("steam"))
        return QUrl(QStringLiteral("https://steamcommunity.com/profiles/%1").arg(id));
    if (type == QStringLiteral("instagram"))
        return QUrl(QStringLiteral("https://www.instagram.com/%1").arg(name));
    if (type == QStringLiteral("tiktok"))
        return QUrl(QStringLiteral("https://www.tiktok.com/@%1").arg(name));
    if (type == QStringLiteral("facebook"))
        return QUrl(QStringLiteral("https://www.facebook.com/%1").arg(id));
    if (type == QStringLiteral("ebay"))
        return QUrl(QStringLiteral("https://www.ebay.com/usr/%1").arg(name));
    if (type == QStringLiteral("domain"))
        return QUrl(QStringLiteral("https://%1").arg(name));
    return {};
}

void clearLayout(QLayout *layout)
{
    while (auto *item = layout->takeAt(0)) {
        if (auto *w = item->widget())
            w->deleteLater();
        delete item;
    }
}

} // namespace

UserProfilePopup::UserProfilePopup(Core::ImageManager *images, Core::ClientInstance *instance,
                                   Core::Snowflake userId, Core::Snowflake guildId,
                                   QWidget *parent)
    : QDialog(parent), images(images), instance(instance), userId(userId), guildId(guildId)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    setWindowTitle(tr("User Profile"));
    guildView = guildId.isValid();

    buildUi();
    renderFromCachedData();
    loadCachedNote();
    requestProfile();

    if (instance) {
        connect(instance->relationships(), &Core::RelationshipManager::relationshipChanged, this,
                [this](Core::Snowflake id) {
                    if (id == this->userId)
                        renderFriendStatus();
                });
        connect(instance->users(), &Core::UserManager::noteChanged, this,
                [this](Core::Snowflake id) {
                    if (id == this->userId)
                        loadCachedNote();
                });
        connect(instance, &QObject::destroyed, this, &QDialog::close);
    }
}

UserProfilePopup::~UserProfilePopup() = default;

void UserProfilePopup::buildUi()
{
    setFixedWidth(PopupWidth);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    rootLayout->addWidget(buildHeader());

    auto *thinDivider = new QFrame(this);
    thinDivider->setFrameShape(QFrame::HLine);
    thinDivider->setStyleSheet(QStringLiteral("color: palette(mid); background-color: palette(mid);"));
    thinDivider->setFixedHeight(1);
    auto *dividerWrap = new QWidget(this);
    auto *dividerWrapLayout = new QHBoxLayout(dividerWrap);
    dividerWrapLayout->setContentsMargins(ContentSidePadding, 0, ContentSidePadding, 0);
    dividerWrapLayout->addWidget(thinDivider);
    rootLayout->addWidget(dividerWrap);

    rootLayout->addWidget(buildBody());
}

QWidget *UserProfilePopup::buildHeader()
{
    headerWidget = new QWidget(this);
    auto *headerLayout = new QVBoxLayout(headerWidget);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(0);

    bannerLabel = new CroppedImageLabel(headerWidget);
    bannerLabel->setFixedHeight(BannerHeight);
    bannerLabel->setAutoFillBackground(true);
    {
        QPalette pal = bannerLabel->palette();
        pal.setColor(QPalette::Window, palette().color(QPalette::Highlight));
        bannerLabel->setPalette(pal);
    }
    headerLayout->addWidget(bannerLabel);

    auto *identityRow = new QWidget(headerWidget);
    identityRow->setMinimumHeight(AvatarSize - AvatarOverlap);
    auto *identityRowLayout = new QHBoxLayout(identityRow);
    identityRowLayout->setContentsMargins(
            ContentSidePadding + AvatarSize + IdentityGap, 8, ContentSidePadding, 0);
    identityRowLayout->setSpacing(0);

    auto *identityCol = new QVBoxLayout;
    identityCol->setContentsMargins(0, 0, 0, 0);
    identityCol->setSpacing(2);

    auto *nameRow = new QHBoxLayout;
    nameRow->setContentsMargins(0, 0, 0, 0);
    nameRow->setSpacing(8);
    displayNameLabel = new QLabel(identityRow);
    {
        QFont f = displayNameLabel->font();
        f.setBold(true);
        f.setPointSize(f.pointSize() + 5);
        displayNameLabel->setFont(f);
    }
    nameRow->addWidget(displayNameLabel, 0);

    botBadgeLabel = new QLabel(QStringLiteral("BOT"), identityRow);
    botBadgeLabel->setStyleSheet(QStringLiteral(
            "background-color: palette(highlight); color: palette(highlighted-text); "
            "font-size: 9px; font-weight: bold; padding: 2px 6px; border-radius: 3px;"));
    botBadgeLabel->setVisible(false);
    botBadgeLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    nameRow->addWidget(botBadgeLabel, 0, Qt::AlignVCenter);

    badgesRow = new QWidget(identityRow);
    auto *badgesLayout = new QHBoxLayout(badgesRow);
    badgesLayout->setContentsMargins(0, 0, 0, 0);
    badgesLayout->setSpacing(3);
    badgesRow->setVisible(false);
    nameRow->addWidget(badgesRow, 0, Qt::AlignVCenter);

    viewToggle = new QPushButton(identityRow);
    viewToggle->setVisible(guildId.isValid());
    connect(viewToggle, &QPushButton::clicked, this, [this]() {
        guildView = !guildView;
        if (hasProfile)
            renderFromProfile();
        else
            renderFromCachedData();
    });
    nameRow->addStretch(1);
    nameRow->addWidget(viewToggle, 0, Qt::AlignVCenter);
    identityCol->addLayout(nameRow);

    handleLabel = new QLabel(identityRow);
    handleLabel->setStyleSheet(QStringLiteral("color: palette(placeholder-text); font-size: 13px;"));
    identityCol->addWidget(handleLabel);
    identityRowLayout->addLayout(identityCol);

    headerLayout->addWidget(identityRow);

    // avatar sits on top of the banner
    avatarLabel = new QLabel(headerWidget);
    avatarLabel->setFixedSize(AvatarSize, AvatarSize);
    avatarLabel->setStyleSheet(QStringLiteral(
                                       "background-color: palette(window); border-radius: %1px;")
                                       .arg(AvatarSize / 2));
    avatarLabel->move(ContentSidePadding, BannerHeight - AvatarOverlap);
    avatarLabel->raise();

    return headerWidget;
}

QWidget *UserProfilePopup::buildBody()
{
    auto *body = new QWidget(this);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(ContentSidePadding, 10, ContentSidePadding, 14);
    bodyLayout->setSpacing(ColumnSpacing);

    auto *leftContent = new QWidget;
    auto *leftCol = new QVBoxLayout(leftContent);
    leftCol->setContentsMargins(0, 0, 6, 0);
    leftCol->setSpacing(10);
    auto *rightCol = new QVBoxLayout;
    rightCol->setContentsMargins(0, 0, 0, 0);
    rightCol->setSpacing(10);

    const QColor headingColor = palette().color(QPalette::PlaceholderText);

    bioSection = new QWidget(body);
    auto *bioLayout = new QVBoxLayout(bioSection);
    bioLayout->setContentsMargins(0, 0, 0, 0);
    bioLayout->setSpacing(4);
    auto *bioHeading = new QLabel(bioSection);
    bioHeading->setText(sectionHeading(tr("About Me"), headingColor));
    bioHeading->setTextFormat(Qt::RichText);
    bioLayout->addWidget(bioHeading);
    bioLabel = new QLabel(bioSection);
    bioLabel->setTextFormat(Qt::PlainText);
    bioLabel->setWordWrap(true);
    bioLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bioLabel->setStyleSheet(QStringLiteral("font-size: 12px;"));
    bioLayout->addWidget(bioLabel);
    bioSection->setVisible(false);
    leftCol->addWidget(bioSection);

    auto buildInfoRow = [&](const QString &emoji, QLabel *&labelSlot, bool muted) {
        auto *row = new QWidget(body);
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(6);
        auto *icon = new QLabel(emoji, row);
        icon->setFixedWidth(18);
        rl->addWidget(icon);
        labelSlot = new QLabel(row);
        labelSlot->setStyleSheet(
                muted ? QStringLiteral("color: palette(placeholder-text); font-size: 12px;")
                      : QStringLiteral("font-size: 12px;"));
        labelSlot->setWordWrap(true);
        rl->addWidget(labelSlot, 1);
        return row;
    };

    auto *infoBlock = new QWidget(body);
    auto *infoBlockLayout = new QVBoxLayout(infoBlock);
    infoBlockLayout->setContentsMargins(0, 0, 0, 0);
    infoBlockLayout->setSpacing(4);
    serverJoinRow = buildInfoRow(QStringLiteral("🪪"), serverJoinLabel, false);
    infoBlockLayout->addWidget(serverJoinRow);
    infoBlockLayout->addWidget(buildInfoRow(QStringLiteral("💬"), discordJoinLabel, false));
    infoBlockLayout->addWidget(buildInfoRow(QStringLiteral("🤝"), friendsSinceLabel, true));
    leftCol->addWidget(infoBlock);

    rolesSection = new QWidget(leftContent);
    auto *rolesLayout = new QVBoxLayout(rolesSection);
    rolesLayout->setContentsMargins(0, 0, 0, 0);
    rolesLayout->setSpacing(4);
    auto *rolesHeading = new QLabel(rolesSection);
    rolesHeading->setText(sectionHeading(tr("Roles"), headingColor));
    rolesHeading->setTextFormat(Qt::RichText);
    rolesLayout->addWidget(rolesHeading);
    rolesLabel = new QLabel(rolesSection);
    rolesLabel->setTextFormat(Qt::RichText);
    rolesLabel->setWordWrap(true);
    rolesLabel->setStyleSheet(QStringLiteral("font-size: 12px;"));
    rolesLayout->addWidget(rolesLabel);
    leftCol->addWidget(rolesSection);

    auto *connectionsSection = new QWidget(leftContent);
    auto *cLayout = new QVBoxLayout(connectionsSection);
    cLayout->setContentsMargins(0, 0, 0, 0);
    cLayout->setSpacing(4);
    auto *cHeading = new QLabel(connectionsSection);
    cHeading->setText(sectionHeading(tr("Connections"), headingColor));
    cHeading->setTextFormat(Qt::RichText);
    cLayout->addWidget(cHeading);
    auto *cInner = new QWidget(connectionsSection);
    connectionsLayout = new QVBoxLayout(cInner);
    connectionsLayout->setContentsMargins(0, 0, 0, 0);
    connectionsLayout->setSpacing(4);
    cLayout->addWidget(cInner);
    leftCol->addWidget(connectionsSection);

    if (!isBot()) {
        auto *noteSection = new QWidget(leftContent);
        auto *noteLayout = new QVBoxLayout(noteSection);
        noteLayout->setContentsMargins(0, 0, 0, 0);
        noteLayout->setSpacing(4);
        auto *noteHeading = new QLabel(noteSection);
        noteHeading->setText(sectionHeading(tr("Note"), headingColor));
        noteHeading->setTextFormat(Qt::RichText);
        noteLayout->addWidget(noteHeading);
        noteEdit = new QTextEdit(noteSection);
        noteEdit->setPlaceholderText(tr("Click to add a note"));
        noteEdit->setFixedHeight(72);
        noteEdit->setStyleSheet(QStringLiteral(
                "QTextEdit { background: palette(base); color: palette(text); border: none; "
                "border-radius: 4px; padding: 6px; font-size: 12px; }"));
        connect(noteEdit, &QTextEdit::textChanged, this, [this]() {
            if (noteLoaded)
                noteDirty = true;
        });
        noteEdit->installEventFilter(this);
        noteLayout->addWidget(noteEdit);
        leftCol->addWidget(noteSection);
    }

    leftCol->addStretch(1);

    auto *leftScroll = new QScrollArea(body);
    leftScroll->setFrameShape(QFrame::NoFrame);
    leftScroll->setWidgetResizable(true);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
    leftContent->setStyleSheet(QStringLiteral("background: transparent;"));
    leftScroll->setWidget(leftContent);
    leftScroll->setFixedHeight(LeftRightColumnHeight);

    rightTabs = new QTabWidget(body);
    rightTabs->setFixedHeight(LeftRightColumnHeight);
    rightTabs->setDocumentMode(true);
    rightTabs->setStyleSheet(QStringLiteral(
            "QTabWidget::pane { border: 1px solid palette(mid); border-radius: 4px; "
            "background: palette(base); top: -1px; }"
            "QTabBar::tab { padding: 5px 10px; font-size: 11px; "
            "background: transparent; color: palette(placeholder-text); border: none; }"
            "QTabBar::tab:selected { color: palette(text); "
            "border-bottom: 2px solid palette(highlight); }"
            "QTabBar::tab:hover:!selected { color: palette(text); }"));

    auto makeScrollableTab = [&](QVBoxLayout *&outLayout) {
        auto *scroll = new QScrollArea(rightTabs);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *content = new QWidget;
        outLayout = new QVBoxLayout(content);
        outLayout->setContentsMargins(8, 8, 8, 8);
        outLayout->setSpacing(4);
        scroll->setWidget(content);
        return scroll;
    };

    mutualServersTab = makeScrollableTab(mutualServersLayout);
    mutualFriendsTab = makeScrollableTab(mutualFriendsLayout);

    rightTabs->addTab(mutualServersTab, tr("Mutual Servers"));
    if (!isBot())
        rightTabs->addTab(mutualFriendsTab, tr("Mutual Friends"));

    rightCol->addWidget(rightTabs);
    rightCol->addStretch(1);

    bodyLayout->addWidget(leftScroll, 1);
    bodyLayout->addLayout(rightCol, 1);
    return body;
}

bool UserProfilePopup::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == noteEdit && ev->type() == QEvent::FocusOut) {
        saveNote();
    }
    return QDialog::eventFilter(obj, ev);
}

void UserProfilePopup::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    positionOverParent();
}

void UserProfilePopup::positionOverParent()
{
    QWidget *anchor = parentWidget() ? parentWidget()->window() : nullptr;
    QRect target;
    if (anchor) {
        target = anchor->geometry();
    } else if (auto *screen = QGuiApplication::primaryScreen()) {
        target = screen->availableGeometry();
    } else {
        return;
    }
    move(target.center() - QPoint(width() / 2, height() / 2));
}

void UserProfilePopup::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        saveNote();
        close();
        return;
    }
    QDialog::keyPressEvent(event);
}

void UserProfilePopup::closeEvent(QCloseEvent *event)
{
    saveNote();
    QDialog::closeEvent(event);
}

void UserProfilePopup::renderFromCachedData()
{
    QString name = resolvedDisplayName();
    setWindowTitle(name);
    displayNameLabel->setText(name.toHtmlEscaped());
    handleLabel->setText(tagHandle().toHtmlEscaped());
    botBadgeLabel->setVisible(isBot());

    int color = resolvedAccentColor();
    if (color != 0) {
        QColor c = QColor::fromRgb(color);
        QPalette pal = bannerLabel->palette();
        pal.setColor(QPalette::Window, c);
        bannerLabel->setPalette(pal);
    }

    QString avatarHash = resolvedAvatarHash();
    Core::Snowflake owner = resolvedGuildAvatarOwner();
    QUrl url = owner.isValid()
                       ? Discord::Cdn::guildMemberAvatar(guildId, userId, avatarHash, 256)
                       : Discord::Cdn::userAvatar(userId, avatarHash, 256);
    images->assign(avatarLabel, url, QSize(AvatarSize, AvatarSize));

    discordJoinLabel->setText(tr("Joined Discord on %1").arg(formatDate(userId.toDateTime())));

    if (guildId.isValid() && instance) {
        QDateTime joinedAt;
        auto member = cachedMember();
        if (member && member->joinedAt.hasValue())
            joinedAt = member->joinedAt.get();
        else if (hasProfile && profile.guildMember.hasValue() && profile.guildMember.get().joinedAt.hasValue())
            joinedAt = profile.guildMember.get().joinedAt.get();

        if (joinedAt.isValid())
            serverJoinLabel->setText(
                    tr("Joined this server on %1").arg(formatDate(joinedAt)));
        else
            serverJoinLabel->setText(tr("Joined this server on —"));
        serverJoinRow->setVisible(true);
    } else {
        serverJoinRow->setVisible(false);
    }

    if (isBot()) {
        friendsSinceLabel->parentWidget()->setVisible(false);
    } else {
        renderFriendStatus();
    }

    applyView();
}

void UserProfilePopup::renderFromProfile()
{
    if (!hasProfile)
        return;

    renderBannerAndBio();
    renderBadges();
    renderFromCachedData();
    renderConnections();
    renderMutualServers();
    renderMutualFriends();
    applyView();
}

void UserProfilePopup::renderBannerAndBio()
{
    QString bannerHash;
    if (profile.user.get().banner.hasValue() && !profile.user.get().banner.get().isEmpty())
        bannerHash = profile.user.get().banner.get();
    if (guildView && profile.guildMemberProfile.hasValue() && profile.guildMemberProfile.get().banner.hasValue() && !profile.guildMemberProfile.get().banner.get().isEmpty())
        bannerHash = profile.guildMemberProfile.get().banner.get();

    if (!bannerHash.isEmpty()) {
        QUrl bannerUrl = Discord::Cdn::userBanner(userId, bannerHash, 1024);
        images->assign(bannerLabel, bannerUrl, QSize(1024, 1024));
    }

    QString bio;
    if (guildView && profile.guildMemberProfile.hasValue() && profile.guildMemberProfile.get().bio.hasValue() && !profile.guildMemberProfile.get().bio.get().isEmpty())
        bio = profile.guildMemberProfile.get().bio.get();
    else if (profile.userProfile.hasValue() && profile.userProfile.get().bio.hasValue() && !profile.userProfile.get().bio.get().isEmpty())
        bio = profile.userProfile.get().bio.get();

    if (bio.isEmpty()) {
        bioSection->setVisible(false);
    } else {
        bioLabel->setText(bio);
        bioSection->setVisible(true);
    }
}

void UserProfilePopup::renderBadges()
{
    auto *badgesLayout = qobject_cast<QHBoxLayout *>(badgesRow->layout());
    if (!badgesLayout)
        return;

    clearLayout(badgesLayout);
    if (!profile.badges.hasValue() || profile.badges.get().isEmpty()) {
        badgesRow->setVisible(false);
        return;
    }

    constexpr int BadgeSize = 18;
    for (const auto &b : profile.badges.get()) {
        if (!b.icon.hasValue() || b.icon.get().isEmpty())
            continue;
        auto *icon = new QLabel(badgesRow);
        icon->setFixedSize(BadgeSize, BadgeSize);
        if (b.description.hasValue())
            icon->setToolTip(b.description.get());
        images->assign(icon, Discord::Cdn::badgeIcon(b.icon.get(), 64),
                       QSize(BadgeSize, BadgeSize));
        badgesLayout->addWidget(icon);
    }
    badgesRow->setVisible(badgesLayout->count() > 0);
}

void UserProfilePopup::renderConnections()
{
    clearLayout(connectionsLayout);
    if (!profile.connectedAccounts.hasValue() || profile.connectedAccounts.get().isEmpty()) {
        auto *empty = new QLabel(tr("No connections"));
        empty->setStyleSheet(QStringLiteral(
                "color: palette(placeholder-text); font-size: 12px;"));
        connectionsLayout->addWidget(empty);
        return;
    }

    const QString linkColor = palette().color(QPalette::Link).name();
    const QString mutedColor = palette().color(QPalette::PlaceholderText).name();
    for (const auto &c : profile.connectedAccounts.get()) {
        auto *row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);

        auto *iconLabel = new QLabel(row);
        iconLabel->setFixedSize(16, 16);
        const QUrl iconUrl = Discord::Cdn::connectionIcon(c.type.get());
        if (iconUrl.isValid())
            images->assign(iconLabel, iconUrl, QSize(16, 16));
        rowLayout->addWidget(iconLabel);

        QUrl link = buildConnectionUrl(c.type.get(), c.id.get(), c.name.get());
        auto *label = new QLabel(row);
        label->setTextFormat(Qt::RichText);
        label->setOpenExternalLinks(false);
        label->setStyleSheet(QStringLiteral("font-size: 12px;"));
        QString typePretty = prettifyConnectionType(c.type.get()).toHtmlEscaped();
        QString name = c.name.get().toHtmlEscaped();
        if (link.isValid()) {
            label->setText(QStringLiteral(
                                   "<a href=\"%1\" style=\"color:%2; text-decoration:none;\">%3</a> "
                                   "<span style=\"color:%4;\">· %5</span>")
                                   .arg(link.toString(), linkColor, name, mutedColor,
                                        typePretty));
            connect(label, &QLabel::linkActivated, this,
                    [](const QString &url) { QDesktopServices::openUrl(QUrl(url)); });
        } else {
            label->setText(QStringLiteral("%1 <span style=\"color:%2;\">· %3</span>")
                                   .arg(name, mutedColor, typePretty));
        }
        rowLayout->addWidget(label, 1);

        connectionsLayout->addWidget(row);
    }
}

void UserProfilePopup::renderMutualServers()
{
    clearLayout(mutualServersLayout);
    if (!profile.mutualGuilds.hasValue() || profile.mutualGuilds.get().isEmpty())
        return;

    constexpr int GuildIconSize = 22;
    const QString rowStyle = QStringLiteral("font-size: 12px;");
    for (const auto &g : profile.mutualGuilds.get()) {
        QString name;
        QString iconHash;
        if (instance) {
            auto guildOpt = instance->getGuild(g.id);
            if (guildOpt) {
                if (guildOpt->name.hasValue())
                    name = guildOpt->name.get();
                if (guildOpt->icon.hasValue())
                    iconHash = guildOpt->icon.get();
            }
        }
        if (name.isEmpty())
            name = tr("Unknown server");

        auto *row = new QWidget;
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 2, 0, 2);
        rl->setSpacing(8);

        auto *icon = new QLabel(row);
        icon->setFixedSize(GuildIconSize, GuildIconSize);
        icon->setStyleSheet(QStringLiteral(
                                    "background-color: palette(button); border-radius: %1px;")
                                    .arg(GuildIconSize / 2));
        icon->setAlignment(Qt::AlignCenter);
        if (!iconHash.isEmpty()) {
            images->assign(icon, Discord::Cdn::guildIcon(g.id, iconHash, 64), QSize(GuildIconSize, GuildIconSize));
        } else {
            icon->setText(name.left(1).toUpper());
            icon->setStyleSheet(icon->styleSheet() + QStringLiteral(" color: palette(text); font-weight: bold;"));
        }
        rl->addWidget(icon);

        auto *nameLabel = new QLabel(name.toHtmlEscaped(), row);
        nameLabel->setStyleSheet(rowStyle);
        rl->addWidget(nameLabel, 1);

        mutualServersLayout->addWidget(row);
    }
    mutualServersLayout->addStretch(1);
}

void UserProfilePopup::renderMutualFriends()
{
    clearLayout(mutualFriendsLayout);
    if (isBot() || !profile.mutualFriends.hasValue() || profile.mutualFriends.get().isEmpty())
        return;

    for (const auto &u : profile.mutualFriends.get()) {
        auto *l = new QLabel(u.getDisplayName().toHtmlEscaped());
        l->setStyleSheet(QStringLiteral("font-size: 12px;"));
        mutualFriendsLayout->addWidget(l);
    }
    mutualFriendsLayout->addStretch(1);
}

void UserProfilePopup::applyView()
{
    const bool canGuildView = guildId.isValid();
    viewToggle->setVisible(canGuildView);
    if (canGuildView)
        viewToggle->setText(guildView ? tr("View main profile") : tr("View server profile"));

    serverJoinRow->setVisible(canGuildView && guildView);
    rolesSection->setVisible(canGuildView && guildView);

    if (!canGuildView || !guildView)
        return;

    if (!instance) {
        rolesSection->setVisible(false);
        return;
    }

    QList<Discord::Role> memberRoles = instance->getMemberRolesSorted(guildId, userId);
    if (memberRoles.isEmpty() && hasProfile && profile.guildMember.hasValue() && profile.guildMember.get().roles.hasValue()) {
        const auto allRoles = instance->getRolesForGuild(guildId);
        const auto &roleIds = profile.guildMember.get().roles.get();
        QSet<Core::Snowflake> roleIdSet(roleIds.begin(), roleIds.end());
        for (const auto &role : allRoles) {
            if (roleIdSet.contains(role.id.get()))
                memberRoles.append(role);
        }
        std::sort(memberRoles.begin(), memberRoles.end(),
                  [](const Discord::Role &a, const Discord::Role &b) {
                      return a.position.get() > b.position.get();
                  });
    }

    if (memberRoles.isEmpty()) {
        rolesLabel->setText(QStringLiteral("<span style=\"color:%1;\">%2</span>")
                                    .arg(palette().color(QPalette::PlaceholderText).name(),
                                         tr("No roles")));
        return;
    }

    const QColor defaultRoleColor = palette().color(QPalette::Text);
    QStringList chips;
    for (const auto &role : memberRoles) {
        QColor color = role.hasColor() ? role.getColor() : defaultRoleColor;
        chips << QStringLiteral(
                         "<span style=\"color:%1; background-color:rgba(%2,%3,%4,40); "
                         "padding:2px 7px; border-radius:6px;\">%5</span>")
                         .arg(color.name())
                         .arg(color.red())
                         .arg(color.green())
                         .arg(color.blue())
                         .arg(role.name.get().toHtmlEscaped());
    }
    rolesLabel->setText(chips.join(QStringLiteral("&nbsp; ")));
}

void UserProfilePopup::saveNote()
{
    if (!noteDirty || !noteLoaded || !instance || isBot())
        return;
    instance->discord()->setUserNote(userId, noteEdit->toPlainText());
    noteDirty = false;
}

void UserProfilePopup::requestProfile()
{
    if (!instance)
        return;
    QPointer<UserProfilePopup> self(this);
    instance->discord()->fetchUserProfile(
            userId, guildId,
            [self](const Core::Result<Discord::UserProfile> &result) {
                if (!self || !result.success())
                    return;
                self->profile = result.value.value();
                self->hasProfile = true;
                self->renderFromProfile();
            });
}

void UserProfilePopup::loadCachedNote()
{
    if (!instance || !noteEdit || isBot())
        return;
    const QString cached = instance->users()->getCachedNote(userId).value_or(QString());
    if (noteLoaded && cached == noteEdit->toPlainText())
        return;
    noteEdit->setPlainText(cached);
    noteLoaded = true;
    noteDirty = false;
}

void UserProfilePopup::renderFriendStatus()
{
    if (!instance || !friendsSinceLabel)
        return;

    auto rel = instance->relationships()->getRelationship(userId);
    const auto type = (rel.has_value() && rel->type.hasValue()) ? rel->type.get()
                                                                : Discord::RelationshipType::NONE;

    QString text;
    bool emphasized = false;

    switch (type) {
    case Discord::RelationshipType::FRIEND:
        text = (rel->since.hasValue() && rel->since.get().isValid())
                       ? tr("Friends since %1").arg(formatDate(rel->since.get()))
                       : tr("Friends");
        emphasized = true;
        break;
    case Discord::RelationshipType::INCOMING_REQUEST:
        text = tr("Friend request pending");
        emphasized = true;
        break;
    case Discord::RelationshipType::OUTGOING_REQUEST:
        text = tr("Friend request sent");
        break;
    case Discord::RelationshipType::BLOCKED:
        text = tr("Blocked");
        break;
    default:
        text = tr("Not friends");
        break;
    }

    const QColor color = palette().color(emphasized ? QPalette::Text : QPalette::PlaceholderText);
    friendsSinceLabel->setText(text);
    friendsSinceLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(color.name()));
}

std::optional<Discord::User> UserProfilePopup::cachedUser() const
{
    if (!instance)
        return std::nullopt;
    return instance->users()->getUser(userId);
}

std::optional<Discord::Member> UserProfilePopup::cachedMember() const
{
    if (!instance || !guildId.isValid())
        return std::nullopt;
    return instance->users()->getMember(guildId, userId);
}

QString UserProfilePopup::tagHandle() const
{
    if (auto u = cachedUser(); u && u->username.hasValue())
        return QStringLiteral("@") + u->username.get();
    if (hasProfile && profile.user.get().username.hasValue())
        return QStringLiteral("@") + profile.user.get().username.get();
    return QStringLiteral("@") + tr("unknown");
}

QString UserProfilePopup::resolvedDisplayName() const
{
    if (guildView && guildId.isValid() && instance) {
        QString n = instance->users()->getDisplayName(userId, guildId);
        if (!n.isEmpty())
            return n;
    }
    if (auto u = cachedUser())
        return u->getDisplayName();
    if (hasProfile)
        return profile.user.get().getDisplayName();
    return tr("Unknown User");
}

QString UserProfilePopup::resolvedAvatarHash() const
{
    if (guildView) {
        if (auto m = cachedMember(); m && m->avatar.hasValue() && !m->avatar.get().isEmpty())
            return m->avatar.get();
    }
    if (auto u = cachedUser(); u && u->avatar.hasValue() && !u->avatar.get().isEmpty())
        return u->avatar.get();
    if (hasProfile && profile.user.get().avatar.hasValue() && !profile.user.get().avatar.get().isEmpty())
        return profile.user.get().avatar.get();
    return {};
}

Core::Snowflake UserProfilePopup::resolvedGuildAvatarOwner() const
{
    if (guildView) {
        if (auto m = cachedMember(); m && m->avatar.hasValue() && !m->avatar.get().isEmpty())
            return guildId;
    }
    return Core::Snowflake::Invalid;
}

int UserProfilePopup::resolvedAccentColor() const
{
    if (guildView && hasProfile && profile.guildMemberProfile.hasValue() && profile.guildMemberProfile.get().accentColor.hasValue())
        return profile.guildMemberProfile.get().accentColor.get();
    if (hasProfile && profile.userProfile.hasValue() && profile.userProfile.get().accentColor.hasValue())
        return profile.userProfile.get().accentColor.get();
    if (hasProfile && profile.user.get().accentColor.hasValue())
        return profile.user.get().accentColor.get();
    if (auto u = cachedUser(); u && u->accentColor.hasValue())
        return u->accentColor.get();
    return 0;
}

bool UserProfilePopup::isBot() const
{
    if (auto u = cachedUser(); u && u->bot.hasValue() && u->bot.get())
        return true;
    return hasProfile && profile.user.get().bot.hasValue() && profile.user.get().bot.get();
}

} // namespace UI
} // namespace Acheron
