#include "UI/Call/CallParticipantList.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Audio/VoiceSettings.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Theme/Icons.hpp"
#include "UI/ElidedLabel.hpp"
#include "UI/VolumeMeter.hpp"

#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace Acheron {
namespace UI {

using Core::Snowflake;
using Core::Audio::VoiceManager;
using Core::Audio::VoiceSettings;
namespace Icons = Core::Theme::Icons;
using Core::Theme::Token;

namespace {

constexpr int ROW_HEIGHT = 28;
constexpr int AVATAR_PIXELS = 22;
constexpr int RING_WIDTH = 2;
// the member list shape
constexpr qreal AVATAR_RADIUS = 4;
constexpr int STATE_ICON_PIXELS = 14;
constexpr int METER_WIDTH = 48;
constexpr int METER_HEIGHT = 6;
constexpr int VOLUME_SLIDER_WIDTH = 80;
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
            painter.drawRoundedRect(whole.adjusted(RING_WIDTH / 2.0, RING_WIDTH / 2.0, -RING_WIDTH / 2.0, -RING_WIDTH / 2.0), AVATAR_RADIUS + RING_WIDTH / 2.0, AVATAR_RADIUS + RING_WIDTH / 2.0);
        }
        const QRectF inside = whole.adjusted(RING_WIDTH, RING_WIDTH, -RING_WIDTH, -RING_WIDTH);
        QPainterPath shape;
        shape.addRoundedRect(inside, AVATAR_RADIUS, AVATAR_RADIUS);
        painter.setClipPath(shape);
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

class VolumeSlider : public QSlider
{
public:
    explicit VolumeSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent)
    {
        setRange(0, VoiceSettings::MAX_GAIN_PERCENT);
        setFixedWidth(VOLUME_SLIDER_WIDTH);
    }

protected:
    void mouseDoubleClickEvent(QMouseEvent *) override { setValue(VoiceSettings::UNCHANGED_PERCENT); }
};

void keepSpaceWhenHidden(QWidget *widget)
{
    QSizePolicy policy = widget->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    widget->setSizePolicy(policy);
}

} // namespace

struct ParticipantState
{
    bool selfMuted = false;
    bool serverMuted = false;
    bool selfDeafened = false;
    bool serverDeafened = false;
};

class CallParticipantRow : public QWidget
{
public:
    CallParticipantRow(VoiceManager *voice, Snowflake userId, bool self, std::function<void(const QPoint &)> menu, QWidget *parent) : QWidget(parent), voice(voice), userId(userId), menuRequested(std::move(menu))
    {
        setFixedHeight(ROW_HEIGHT);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 0, 6, 0);
        layout->setSpacing(6);
        avatar = new SpeakingAvatar(this);
        layout->addWidget(avatar);
        name = new ElidedLabel(this);
        layout->addWidget(name, 1);
        muted = stateIcon(this, CallParticipantList::tr("Muted"));
        layout->addWidget(muted);
        deafened = stateIcon(this, CallParticipantList::tr("Deafened"));
        layout->addWidget(deafened);
        meter = new VolumeMeter(this);
        meter->setFixedSize(METER_WIDTH, METER_HEIGHT);
        layout->addWidget(meter);

        volume = new VolumeSlider(this);
        layout->addWidget(volume);
        muteButton = new QToolButton(this);
        muteButton->setCheckable(true);
        muteButton->setAutoRaise(true);
        layout->addWidget(muteButton);
        keepSpaceWhenHidden(volume);
        keepSpaceWhenHidden(muteButton);
        if (self) {
            volume->hide();
            muteButton->hide();
            return;
        }
        refreshPlayback();
        connect(volume, &QSlider::valueChanged, this, [this](int percent) {
            if (this->voice)
                this->voice->setUserVolume(this->userId, percent);
        });
        connect(muteButton, &QToolButton::toggled, this, [this](bool mute) {
            if (this->voice)
                this->voice->setUserMuted(this->userId, mute);
        });
    }

    void setName(const QString &text) { name->setText(text); }
    void setAvatar(const QPixmap &pixmap) { avatar->setPixmap(pixmap); }
    void setSpeaking(bool speaking) { avatar->setSpeaking(speaking); }
    void setLevel(float rms) { meter->setLevel(rms); }

    void refreshPlayback()
    {
        if (!voice)
            return;
        const QSignalBlocker quiet(volume);
        volume->setValue(voice->userVolume(userId));
        volume->setToolTip(CallParticipantList::tr("Volume %1%").arg(volume->value()));
        const bool mutedByMe = voice->isUserMuted(userId);
        const QSignalBlocker quietButton(muteButton);
        muteButton->setChecked(mutedByMe);
        muteButton->setIcon(Icons::icon(mutedByMe ? Icons::Name::VolumeOff : Icons::Name::VolumeOn, mutedByMe ? Token::ChatError : Token::ButtonText));
        muteButton->setToolTip(mutedByMe ? CallParticipantList::tr("Unmute for you") : CallParticipantList::tr("Mute for you"));
    }

    void setState(const ParticipantState &state)
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
    }

protected:
    void contextMenuEvent(QContextMenuEvent *event) override { menuRequested(event->globalPos()); }

private:
    QPointer<VoiceManager> voice;
    Snowflake userId;
    std::function<void(const QPoint &)> menuRequested;
    SpeakingAvatar *avatar;
    ElidedLabel *name;
    QLabel *muted;
    QLabel *deafened;
    VolumeMeter *meter;
    VolumeSlider *volume;
    QToolButton *muteButton;
};

CallParticipantList::CallParticipantList(Core::Audio::VoiceManager *voice, Core::ImageManager *images, Snowflake accountId, NameResolver names, AvatarResolver avatars, QWidget *parent)
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

    connect(voice, &VoiceManager::participantSpeakingChanged, this, [this](Snowflake userId, bool speaking) {
        if (CallParticipantRow *row = rowOf(userId))
            row->setSpeaking(speaking);
    });
    connect(voice, &VoiceManager::speakingChanged, this, [this](bool speaking) {
        if (CallParticipantRow *row = rowOf(this->accountId))
            row->setSpeaking(speaking);
    });
    connect(voice, &VoiceManager::userAudioLevelChanged, this, [this](Snowflake userId, float rms) {
        if (CallParticipantRow *row = rowOf(userId))
            row->setLevel(rms);
    });
    connect(voice, &VoiceManager::audioLevelChanged, this, [this](float rms) {
        if (CallParticipantRow *row = rowOf(this->accountId))
            row->setLevel(rms);
    });
    connect(voice, &VoiceManager::selfAudioChanged, this, &CallParticipantList::refresh);
    connect(voice, &VoiceManager::userPlaybackChanged, this, [this](Snowflake userId) {
        if (CallParticipantRow *row = rowOf(userId))
            row->refreshPlayback();
    });
    if (images)
        connect(images, &Core::ImageManager::imageFetched, this, &CallParticipantList::onImageFetched);
}

CallParticipantList::~CallParticipantList() = default;

CallParticipantRow *CallParticipantList::rowOf(Snowflake userId) const
{
    const auto found = rows.find(userId);
    return found == rows.end() ? nullptr : found->second;
}

void CallParticipantList::refresh()
{
    if (!voice)
        return;

    QList<Core::Audio::VoiceParticipant> participants = voice->currentParticipants();
    if (voice->currentChannelId().isValid())
        participants.append({ .userId = accountId, .selfMute = voice->isSelfMuted(), .selfDeaf = voice->isSelfDeafened() });
    const auto nameOf = [this](Snowflake userId) { return names ? names(userId) : QString::number(static_cast<quint64>(userId)); };
    std::sort(participants.begin(), participants.end(), [this, &nameOf](const Core::Audio::VoiceParticipant &a, const Core::Audio::VoiceParticipant &b) {
        if ((a.userId == accountId) != (b.userId == accountId))
            return a.userId == accountId;
        return QString::localeAwareCompare(nameOf(a.userId), nameOf(b.userId)) < 0;
    });

    for (auto it = rows.begin(); it != rows.end();) {
        const bool present = std::any_of(participants.cbegin(), participants.cend(), [&it](const Core::Audio::VoiceParticipant &each) { return each.userId == it->first; });
        if (present) {
            ++it;
            continue;
        }
        delete it->second;
        it = rows.erase(it);
    }

    for (int index = 0; index < participants.size(); index++) {
        const Core::Audio::VoiceParticipant &participant = participants[index];
        CallParticipantRow *row = rowOf(participant.userId);
        if (!row) {
            row = new CallParticipantRow(voice, participant.userId, participant.userId == accountId, [this, userId = participant.userId](const QPoint &globalPos) { emit menuRequested(userId, globalPos); }, widget());
            rows.emplace(participant.userId, row);
            if (images && avatars) {
                const QUrl url = avatars(participant.userId);
                if (url.isValid())
                    row->setAvatar(avatarTracker.fetch(images, url, AVATAR_REQUEST_SIZE, participant.userId, accountId));
            }
        }
        rowsLayout->removeWidget(row);
        rowsLayout->insertWidget(index, row);
        row->setName(nameOf(participant.userId));
        row->setState({ .selfMuted = participant.selfMute, .serverMuted = participant.serverMute, .selfDeafened = participant.selfDeaf, .serverDeafened = participant.serverDeaf });
    }
}

int CallParticipantList::wantedHeight() const
{
    const int shown = std::min(static_cast<int>(rows.size()), MOST_ROWS_SHOWN);
    return shown * ROW_HEIGHT + rowsLayout->contentsMargins().top() + rowsLayout->contentsMargins().bottom();
}

void CallParticipantList::onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap)
{
    if (size != AVATAR_REQUEST_SIZE)
        return;
    avatarTracker.notify(url, [this, &pixmap](Snowflake userId) {
        if (CallParticipantRow *row = rowOf(userId))
            row->setAvatar(pixmap);
    });
}

} // namespace UI
} // namespace Acheron
