#include "ChannelTopicPopup.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>

#include <cmath>

#include "UI/Chat/MessageTextBrowser.hpp"

namespace Acheron {
namespace UI {

namespace {
constexpr int TopicWidth = 480;
constexpr int MaxTopicHeight = 360;
} // namespace

ChannelTopicPopup::ChannelTopicPopup(const QString &channelName, const QString &topicHtml, Core::ImageManager *images, Core::Snowflake accountId, QWidget *parent)
    : BasePopup(parent)
{
    auto *layout = new QVBoxLayout(getContainer());
    layout->setSpacing(15);
    layout->setContentsMargins(24, 24, 24, 24);

    auto *titleLabel = new QLabel(QStringLiteral("#") + channelName, getContainer());
    titleLabel->setTextFormat(Qt::PlainText);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    auto *line = new QFrame(getContainer());
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line);

    auto *topic = new MessageTextBrowser(images, accountId, getContainer());
    topic->setMessageHtml(topicHtml, TopicWidth);
    topic->setFixedSize(TopicWidth, qMin(MaxTopicHeight, int(std::ceil(topic->document()->size().height()))));
    connect(topic, &MessageTextBrowser::linkActivated, this, &ChannelTopicPopup::linkActivated);
    layout->addWidget(topic);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, getContainer());
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

} // namespace UI
} // namespace Acheron
