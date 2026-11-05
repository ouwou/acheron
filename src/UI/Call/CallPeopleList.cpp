#include "UI/Call/CallPeopleList.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Theme/Icons.hpp"
#include "UI/ElidedLabel.hpp"
#include "UI/VolumeMeter.hpp"

#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>

namespace Acheron {
namespace UI {

using Core::Snowflake;
namespace Icons = Core::Theme::Icons;
using Core::Theme::Token;

namespace {

constexpr int ROW_HEIGHT = 28;
constexpr int AVATAR_PIXELS = 22;
constexpr int RING_WIDTH = 2;
constexpr int STATE_ICON_PIXELS = 14;
constexpr int METER_WIDTH = 48;
constexpr int METER_HEIGHT = 6;
constexpr QSize AVATAR_REQUEST_SIZE(32, 32);

class SpeakingAvatar : public QWidget
{
public:
    explicit SpeakingAvatar(QWidget *parent) : QWidget(parent) { setFixedSize(AVATAR_PIXELS + 2 * RING_WIDTH, AVATAR_PIXELS + 2 * RING_WIDTH); }

    void setPixmap(const QPixmap &pixmap)
    {
        avatar = pixmap;
        update();
    }

    void setSpeaking(bool value)
    {
        if (speaking == value)
            return;
        speaking = value;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF whole(rect());
        if (speaking) {
            painter.setPen(QPen(palette().color(QPalette::Highlight), RING_WIDTH));
            painter.drawEllipse(whole.adjusted(RING_WIDTH / 2.0, RING_WIDTH / 2.0, -RING_WIDTH / 2.0, -RING_WIDTH / 2.0));
        }
        const QRectF inside = whole.adjusted(RING_WIDTH, RING_WIDTH, -RING_WIDTH, -RING_WIDTH);
        QPainterPath circle;
        circle.addEllipse(inside);
        painter.setClipPath(circle);
        if (avatar.isNull())
            painter.fillRect(inside, palette().color(QPalette::Mid));
        else
            painter.drawPixmap(inside.toRect(), avatar);
    }

private:
    QPixmap avatar;
    bool speaking = false;
};

QLabel *stateIcon(QWidget *parent, const QString &tooltip)
{
    auto *label = new QLabel(parent);
    label->setToolTip(tooltip);
    label->hide();
    return label;
}

} // namespace

struct PersonState
{
    bool selfMuted = false;
    bool serverMuted = false;
    bool selfDeafened = false;
    bool serverDeafened = false;
    bool mutedByMe = false;
};

class CallPersonRow : public QWidget
{
public:
    CallPersonRow(std::function<void(const QPoint &)> menu, QWidget *parent) : QWidget(parent), menuRequested(std::move(menu))
    {
        setFixedHeight(ROW_HEIGHT);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 0, 6, 0);
        layout->setSpacing(6);
        avatar = new SpeakingAvatar(this);
        layout->addWidget(avatar);
        name = new ElidedLabel(this);
        layout->addWidget(name, 1);
        muted = stateIcon(this, CallPeopleList::tr("Muted"));
        layout->addWidget(muted);
        deafened = stateIcon(this, CallPeopleList::tr("Deafened"));
        layout->addWidget(deafened);
        mutedByMe = stateIcon(this, CallPeopleList::tr("Muted by you"));
        layout->addWidget(mutedByMe);
        meter = new VolumeMeter(this);
        meter->setFixedSize(METER_WIDTH, METER_HEIGHT);
        layout->addWidget(meter);
    }

    void setName(const QString &text) { name->setText(text); }
    void setAvatar(const QPixmap &pixmap) { avatar->setPixmap(pixmap); }
    void setSpeaking(bool speaking) { avatar->setSpeaking(speaking); }
    void setLevel(float rms) { meter->setLevel(rms); }

    void setState(const PersonState &state)
    {
        const qreal ratio = devicePixelRatioF();
        const bool anyMute = state.selfMuted || state.serverMuted;
        muted->setVisible(anyMute);
        if (anyMute)
            muted->setPixmap(Icons::pixmap(Icons::Name::MicOff, STATE_ICON_PIXELS, state.serverMuted ? Token::ChatError : Token::ButtonText, ratio));
        const bool anyDeaf = state.selfDeafened || state.serverDeafened;
        deafened->setVisible(anyDeaf);
        if (anyDeaf)
            deafened->setPixmap(Icons::pixmap(Icons::Name::HeadphoneOff, STATE_ICON_PIXELS, state.serverDeafened ? Token::ChatError : Token::ButtonText, ratio));
        mutedByMe->setVisible(state.mutedByMe);
        if (state.mutedByMe)
            mutedByMe->setPixmap(Icons::pixmap(Icons::Name::VolumeOff, STATE_ICON_PIXELS, Token::ButtonText, ratio));
    }

protected:
    void contextMenuEvent(QContextMenuEvent *event) override { menuRequested(event->globalPos()); }

private:
    std::function<void(const QPoint &)> menuRequested;
    SpeakingAvatar *avatar;
    ElidedLabel *name;
    QLabel *muted;
    QLabel *deafened;
    QLabel *mutedByMe;
    VolumeMeter *meter;
};

CallPeopleList::CallPeopleList(Core::Audio::VoiceManager *voice, Core::ImageManager *images, Snowflake accountId, NameResolver names, AvatarResolver avatars, QWidget *parent)
    : QScrollArea(parent), voice(voice), images(images), accountId(accountId), names(std::move(names)), avatars(std::move(avatars))
{
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget(this);
    rowsLayout = new QVBoxLayout(content);
    rowsLayout->setContentsMargins(0, 2, 0, 2);
    rowsLayout->setSpacing(0);
    rowsLayout->addStretch(1);
    setWidget(content);

    using Core::Audio::VoiceManager;
    connect(voice, &VoiceManager::participantSpeakingChanged, this, [this](Snowflake userId, bool speaking) {
        if (CallPersonRow *row = rowOf(userId))
            row->setSpeaking(speaking);
    });
    connect(voice, &VoiceManager::speakingChanged, this, [this](bool speaking) {
        if (CallPersonRow *row = rowOf(this->accountId))
            row->setSpeaking(speaking);
    });
    connect(voice, &VoiceManager::userAudioLevelChanged, this, [this](Snowflake userId, float rms) {
        if (CallPersonRow *row = rowOf(userId))
            row->setLevel(rms);
    });
    connect(voice, &VoiceManager::audioLevelChanged, this, [this](float rms) {
        if (CallPersonRow *row = rowOf(this->accountId))
            row->setLevel(rms);
    });
    connect(voice, &VoiceManager::selfAudioChanged, this, &CallPeopleList::refresh);
    if (images)
        connect(images, &Core::ImageManager::imageFetched, this, &CallPeopleList::onImageFetched);
}

CallPeopleList::~CallPeopleList() = default;

CallPersonRow *CallPeopleList::rowOf(Snowflake userId) const
{
    const auto found = rows.find(userId);
    return found == rows.end() ? nullptr : found->second;
}

void CallPeopleList::refresh()
{
    if (!voice)
        return;

    QList<Core::Audio::VoiceParticipant> people = voice->currentParticipants();
    if (voice->currentChannelId().isValid())
        people.append({ .userId = accountId, .selfMute = voice->isSelfMuted(), .selfDeaf = voice->isSelfDeafened() });
    const auto nameOf = [this](Snowflake userId) { return names ? names(userId) : QString::number(static_cast<quint64>(userId)); };
    std::sort(people.begin(), people.end(), [this, &nameOf](const Core::Audio::VoiceParticipant &a, const Core::Audio::VoiceParticipant &b) {
        if ((a.userId == accountId) != (b.userId == accountId))
            return a.userId == accountId;
        return QString::localeAwareCompare(nameOf(a.userId), nameOf(b.userId)) < 0;
    });

    for (auto it = rows.begin(); it != rows.end();) {
        const bool present = std::any_of(people.cbegin(), people.cend(), [&it](const Core::Audio::VoiceParticipant &each) { return each.userId == it->first; });
        if (present) {
            ++it;
            continue;
        }
        delete it->second;
        it = rows.erase(it);
    }

    for (int index = 0; index < people.size(); index++) {
        const Core::Audio::VoiceParticipant &person = people[index];
        CallPersonRow *row = rowOf(person.userId);
        if (!row) {
            row = new CallPersonRow([this, userId = person.userId](const QPoint &globalPos) { emit menuRequested(userId, globalPos); }, widget());
            rows.emplace(person.userId, row);
            if (images && avatars) {
                const QUrl url = avatars(person.userId);
                if (url.isValid())
                    row->setAvatar(avatarTracker.fetch(images, url, AVATAR_REQUEST_SIZE, person.userId, accountId));
            }
        }
        rowsLayout->removeWidget(row);
        rowsLayout->insertWidget(index, row);
        row->setName(nameOf(person.userId));
        row->setState({ .selfMuted = person.selfMute, .serverMuted = person.serverMute, .selfDeafened = person.selfDeaf, .serverDeafened = person.serverDeaf, .mutedByMe = person.userId != accountId && voice->isUserMuted(person.userId) });
    }
}

int CallPeopleList::wantedHeight() const
{
    const int shown = std::min(static_cast<int>(rows.size()), MOST_ROWS_SHOWN);
    return shown * ROW_HEIGHT + rowsLayout->contentsMargins().top() + rowsLayout->contentsMargins().bottom();
}

void CallPeopleList::onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap)
{
    if (size != AVATAR_REQUEST_SIZE)
        return;
    avatarTracker.notify(url, [this, &pixmap](Snowflake userId) {
        if (CallPersonRow *row = rowOf(userId))
            row->setAvatar(pixmap);
    });
}

} // namespace UI
} // namespace Acheron
