#include "VoiceStatusBar.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Theme/Icons.hpp"
#include "UI/VoiceActions.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QToolButton>

namespace Acheron {
namespace UI {

namespace Icons = Core::Theme::Icons;

namespace {

const QColor IDLE_ICON(0xb9, 0xbb, 0xbe);
const QColor ALERT_ICON(0xf0, 0x47, 0x47);

QToolButton *barButton(QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setAutoRaise(true);
    button->setIconSize(QSize(16, 16));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

} // namespace

VoiceStatusBar::VoiceStatusBar(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(32);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Show the call"));
    setStyleSheet("QWidget { background-color: #1a1a2e; }"
                  "QLabel { background: transparent; }"
                  "QToolButton { background: transparent; border: none; border-radius: 4px; padding: 2px; }"
                  "QToolButton[popupMode=\"1\"] { padding-right: 12px; }"
                  "QToolButton::menu-button { border: none; width: 10px; }"
                  "QToolButton:hover { background: rgba(255, 255, 255, 20); }");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 0, 4, 0);
    layout->setSpacing(4);

    statusDot = new QLabel(this);
    statusDot->setFixedSize(8, 8);
    layout->addWidget(statusDot);
    statusLabel = new QLabel(this);
    layout->addWidget(statusLabel);
    channelLabel = new QLabel(this);
    channelLabel->setStyleSheet("QLabel { color: #72767d; font-size: 11px; }");
    layout->addWidget(channelLabel, 1);

    privacyButton = barButton(this);
    privacyButton->setIcon(Icons::icon(Icons::Name::Lock, IDLE_ICON));
    privacyButton->setToolTip(tr("End-to-end encrypted. Show the privacy code"));
    connect(privacyButton, &QToolButton::clicked, this, [this] {
        if (voiceManager)
            VoiceActions::showPrivacyCode(this, voiceManager);
    });
    layout->addWidget(privacyButton);

    micButton = barButton(this);
    micButton->setPopupMode(QToolButton::MenuButtonPopup);
    deviceMenu = new QMenu(micButton);
    micButton->setMenu(deviceMenu);
    connect(deviceMenu, &QMenu::aboutToShow, this, [this] {
        if (voiceManager)
            VoiceActions::fillDeviceMenu(deviceMenu, voiceManager, [this] { emit voiceSettingsRequested(); });
    });
    connect(micButton, &QToolButton::clicked, this, [this] {
        if (voiceManager)
            voiceManager->toggleSelfMute();
    });
    layout->addWidget(micButton);

    deafenButton = barButton(this);
    connect(deafenButton, &QToolButton::clicked, this, [this] {
        if (voiceManager)
            voiceManager->toggleSelfDeafen();
    });
    layout->addWidget(deafenButton);

    disconnectButton = barButton(this);
    disconnectButton->setIcon(Icons::icon(Icons::Name::PhoneOff, ALERT_ICON));
    disconnectButton->setToolTip(tr("Disconnect"));
    connect(disconnectButton, &QToolButton::clicked, this, &VoiceStatusBar::disconnectRequested);
    layout->addWidget(disconnectButton);

    updateConnectionState();
}

void VoiceStatusBar::setVoiceManager(Core::Audio::VoiceManager *manager)
{
    if (voiceManager == manager)
        return;
    if (voiceManager)
        disconnect(voiceManager, nullptr, this, nullptr);
    voiceManager = manager;
    if (voiceManager) {
        connect(voiceManager, &Core::Audio::VoiceManager::voiceStateChanged, this, &VoiceStatusBar::updateConnectionState);
        connect(voiceManager, &Core::Audio::VoiceManager::selfAudioChanged, this, &VoiceStatusBar::updateSelfAudio);
        connect(voiceManager, &Core::Audio::VoiceManager::privacyCodeChanged, this, &VoiceStatusBar::updatePrivacy);
    }
    updateConnectionState();
}

void VoiceStatusBar::setChannelName(const QString &name)
{
    channelLabel->setText(name);
}

void VoiceStatusBar::updateConnectionState()
{
    using State = Discord::Voice::VoiceClient::State;
    const State state = voiceManager ? voiceManager->clientState() : State::Disconnected;

    QString text = tr("Disconnected");
    QString colour = QStringLiteral("#999");
    switch (state) {
    case State::Connected:
        text = tr("Voice Connected");
        colour = QStringLiteral("#43b581");
        break;
    case State::Connecting:
        text = tr("Connecting...");
        colour = QStringLiteral("#faa61a");
        break;
    case State::Identifying:
        text = tr("Identifying...");
        colour = QStringLiteral("#faa61a");
        break;
    case State::WaitingForReady:
    case State::DiscoveringIP:
    case State::SelectingProtocol:
    case State::WaitingForSession:
        text = tr("Securing...");
        colour = QStringLiteral("#faa61a");
        break;
    case State::Disconnected:
        break;
    }
    statusDot->setStyleSheet(QStringLiteral("QLabel { background-color: %1; border-radius: 4px; }").arg(colour));
    statusLabel->setText(text);
    statusLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: bold; }").arg(colour));

    setVisible(state != State::Disconnected);
    updateSelfAudio();
    updatePrivacy();
}

void VoiceStatusBar::updateSelfAudio()
{
    const bool muted = voiceManager && voiceManager->isSelfMuted();
    const bool deafened = voiceManager && voiceManager->isSelfDeafened();
    micButton->setIcon(Icons::icon(muted ? Icons::Name::MicOff : Icons::Name::Mic, muted ? ALERT_ICON : IDLE_ICON));
    micButton->setToolTip(muted ? tr("Unmute") : tr("Mute"));
    deafenButton->setIcon(Icons::icon(deafened ? Icons::Name::HeadphoneOff : Icons::Name::Headphones, deafened ? ALERT_ICON : IDLE_ICON));
    deafenButton->setToolTip(deafened ? tr("Undeafen") : tr("Deafen"));
}

void VoiceStatusBar::updatePrivacy()
{
    privacyButton->setVisible(voiceManager && voiceManager->isDaveEnabled() && !voiceManager->privacyCode().isEmpty());
}

void VoiceStatusBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        emit callRevealRequested();
    QWidget::mousePressEvent(event);
}

} // namespace UI
} // namespace Acheron
