#include "ActivityCard.hpp"

#include <QDateTime>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "Core/ImageManager.hpp"
#include "Discord/CdnUrls.hpp"

namespace Acheron {
namespace UI {

constexpr static int LargeImageSize = 60;
constexpr static int SmallImageSize = 22;
constexpr static int ProgressHeight = 4;

using Core::ActivityFormat::TimerInfo;

ActivityCard::ActivityCard(const Discord::Activity &activity, const QUrl &fallbackImage,
                           Core::ImageManager *images, Core::Snowflake accountId, QWidget *parent)
    : QWidget(parent), images(images), accountId(accountId)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(6);

    headingLabel = new QLabel(this);
    QFont headingFont = headingLabel->font();
    headingFont.setPixelSize(11);
    headingFont.setWeight(QFont::DemiBold);
    headingLabel->setFont(headingFont);
    headingLabel->setWordWrap(true);
    root->addWidget(headingLabel);

    auto *row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);

    auto *imageHolder = new QWidget(this);
    imageHolder->setFixedSize(LargeImageSize, LargeImageSize);

    largeImageLabel = new QLabel(imageHolder);
    largeImageLabel->setGeometry(0, 0, LargeImageSize, LargeImageSize);
    largeImageLabel->setScaledContents(true);

    smallImageLabel = new QLabel(imageHolder);
    smallImageLabel->setFixedSize(SmallImageSize, SmallImageSize);
    smallImageLabel->setScaledContents(true);
    smallImageLabel->move(LargeImageSize - SmallImageSize, LargeImageSize - SmallImageSize);
    smallImageLabel->hide();

    row->addWidget(imageHolder, 0, Qt::AlignTop);

    auto *textCol = new QVBoxLayout();
    textCol->setContentsMargins(0, 0, 0, 0);
    textCol->setSpacing(2);

    nameLabel = new QLabel(this);
    QFont nameFont = nameLabel->font();
    nameFont.setPixelSize(13);
    nameFont.setWeight(QFont::DemiBold);
    nameLabel->setFont(nameFont);
    nameLabel->setWordWrap(true);
    textCol->addWidget(nameLabel);

    detailsLabel = new QLabel(this);
    detailsLabel->setWordWrap(true);
    textCol->addWidget(detailsLabel);

    stateLabel = new QLabel(this);
    stateLabel->setWordWrap(true);
    textCol->addWidget(stateLabel);

    progressBar = new QWidget(this);
    progressBar->setFixedHeight(ProgressHeight);
    progressBar->setStyleSheet(QStringLiteral("background: palette(mid); border-radius: 2px;"));
    progressFill = new QWidget(progressBar);
    progressFill->setStyleSheet(QStringLiteral("background: palette(highlight); border-radius: 2px;"));
    progressFill->setGeometry(0, 0, 0, ProgressHeight);
    progressBar->installEventFilter(this);
    progressBar->hide();
    textCol->addWidget(progressBar);

    timerLabel = new QLabel(this);
    textCol->addWidget(timerLabel);

    textCol->addStretch(1);
    row->addLayout(textCol, 1);
    root->addLayout(row);

    for (QLabel *label : { detailsLabel, stateLabel, timerLabel }) {
        QFont font = label->font();
        font.setPixelSize(11);
        label->setFont(font);
    }

    applyActivity(activity, fallbackImage);
}

bool ActivityCard::hasTimer() const
{
    return timer.kind != TimerInfo::Kind::None;
}

void ActivityCard::applyActivity(const Discord::Activity &activity, const QUrl &fallbackImage)
{
    headingLabel->setText(Core::ActivityFormat::cardHeading(activity));

    const bool named = Core::ActivityFormat::headingHasName(activity);
    const QString title = named ? activity.detailsText() : activity.nameText();
    const QString details = named ? QString() : activity.detailsText();

    nameLabel->setText(title);
    nameLabel->setVisible(!title.isEmpty());

    detailsLabel->setText(details);
    detailsLabel->setVisible(!details.isEmpty());

    const QString state = Core::ActivityFormat::partyText(activity);
    stateLabel->setText(state);
    stateLabel->setVisible(!state.isEmpty());

    const Core::Snowflake applicationId = activity.applicationId.hasValue() ? activity.applicationId.get() : Core::Snowflake();

    QUrl largeUrl;
    QUrl smallUrl;
    if (activity.assets.hasValue()) {
        const Discord::ActivityAssets &assets = activity.assets.get();
        if (assets.largeImage.hasValue())
            largeUrl = Discord::Cdn::activityAsset(applicationId, assets.largeImage.get(), 160);
        if (assets.smallImage.hasValue() && !activity.hasFlag(Discord::ActivityFlag::EMBEDDED))
            smallUrl = Discord::Cdn::activityAsset(applicationId, assets.smallImage.get(), 64);

        if (assets.largeText.hasValue())
            largeImageLabel->setToolTip(assets.largeText.get());
        if (assets.smallText.hasValue())
            smallImageLabel->setToolTip(assets.smallText.get());
    }

    if (largeUrl.isEmpty())
        largeUrl = fallbackImage;

    if (largeUrl.isEmpty()) {
        largeImageLabel->setPixmap(QPixmap());
        largeImageLabel->setStyleSheet(QStringLiteral("background: palette(mid); border-radius: 6px;"));
    } else {
        largeImageLabel->setStyleSheet(QString());
        images->assign(largeImageLabel, largeUrl, QSize(LargeImageSize, LargeImageSize), accountId);
    }

    if (smallUrl.isEmpty()) {
        smallImageLabel->hide();
    } else {
        images->assign(smallImageLabel, smallUrl, QSize(SmallImageSize, SmallImageSize), accountId);
        smallImageLabel->show();
    }

    timer = Core::ActivityFormat::timerFor(activity);
    progressBar->setVisible(timer.kind == TimerInfo::Kind::Progress);
    timerLabel->setVisible(timer.kind != TimerInfo::Kind::None);

    tick(QDateTime::currentMSecsSinceEpoch());
}

void ActivityCard::tick(qint64 nowMs)
{
    if (timer.kind == TimerInfo::Kind::None)
        return;

    timerLabel->setText(Core::ActivityFormat::timerText(timer, nowMs));

    if (timer.kind == TimerInfo::Kind::Progress) {
        fillRatio = Core::ActivityFormat::progress(timer, nowMs);
        updateProgressFill();
    }
}

void ActivityCard::updateProgressFill()
{
    progressFill->setGeometry(0, 0, static_cast<int>(progressBar->width() * fillRatio), ProgressHeight);
}

bool ActivityCard::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == progressBar && event->type() == QEvent::Resize)
        updateProgressFill();

    return QWidget::eventFilter(watched, event);
}

} // namespace UI
} // namespace Acheron
