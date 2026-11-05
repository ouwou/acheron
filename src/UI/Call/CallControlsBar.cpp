#include "UI/Call/CallControlsBar.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Streams/StreamBroadcaster.hpp"
#include "Core/Theme/Icons.hpp"
#include "Core/Video/CameraPipeline.hpp"
#include "Core/Video/CameraSender.hpp"
#include "Core/Video/CaptureSources.hpp"
#include "UI/ElidedLabel.hpp"
#include "UI/ScreenSharePicker.hpp"
#include "UI/VoiceActions.hpp"

#include <QActionGroup>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>

namespace Acheron {
namespace UI {

using Core::CameraSender;
using Core::StreamBroadcaster;
using Core::Video::SendQuality;
namespace Icons = Core::Theme::Icons;
using Core::Theme::Token;

namespace {

QToolButton *callButton(QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    return button;
}

void changeShareQuality(StreamBroadcaster *broadcaster, const SendQuality &quality)
{
    broadcaster->setQuality(quality);
    ScreenSharePicker::rememberQuality(broadcaster->quality());
}

} // namespace

void fillShareQualityMenu(QMenu *menu, StreamBroadcaster *broadcaster)
{
    menu->clear();
    if (!broadcaster)
        return;

    const Core::Video::SharerTier tier = broadcaster->tier();
    const SendQuality current = broadcaster->quality();

    auto *heights = new QActionGroup(menu);
    for (const int height : Core::Video::SEND_HEIGHTS) {
        const SendQuality there = Core::Video::atHeight(current, height, tier);
        QAction *action = menu->addAction(ScreenSharePicker::heightLabel(height));
        action->setCheckable(true);
        action->setChecked(height == current.height);
        action->setEnabled(Core::Video::mayUseQuality(there, tier));
        heights->addAction(action);
        QObject::connect(action, &QAction::triggered, broadcaster, [broadcaster, there] { changeShareQuality(broadcaster, there); });
    }

    menu->addSeparator();
    auto *framerates = new QActionGroup(menu);
    for (const int framerate : Core::Video::SEND_FRAMERATES) {
        const SendQuality atRate{ current.height, framerate };
        QAction *action = menu->addAction(ScreenSharePicker::framerateLabel(framerate));
        action->setCheckable(true);
        action->setChecked(framerate == current.framerate);
        action->setEnabled(Core::Video::mayUseQuality(atRate, tier));
        framerates->addAction(action);
        QObject::connect(action, &QAction::triggered, broadcaster, [broadcaster, atRate] { changeShareQuality(broadcaster, atRate); });
    }
}

CallControlsBar::CallControlsBar(Core::Audio::VoiceManager *voice, StreamBroadcaster *broadcaster, CameraSender *camera, bool nonVideoShown, QWidget *parent)
    : QWidget(parent), voice(voice), broadcaster(broadcaster), camera(camera)
{
    auto *bar = new QHBoxLayout(this);
    bar->setContentsMargins(4, 2, 4, 2);
    bar->setSpacing(8);

    collapseButton = new QToolButton(this);
    collapseButton->setAutoRaise(true);
    bar->addWidget(collapseButton);
    connect(collapseButton, &QToolButton::clicked, this, &CallControlsBar::collapseClicked);

    summaryLabel = new ElidedLabel(QString(), this);
    bar->addWidget(summaryLabel, 1);

    micButton = callButton(this);
    micButton->setPopupMode(QToolButton::MenuButtonPopup);
    deviceMenu = new QMenu(micButton);
    micButton->setMenu(deviceMenu);
    bar->addWidget(micButton);
    connect(micButton, &QToolButton::clicked, this, [this] {
        if (this->voice)
            this->voice->toggleSelfMute();
    });
    connect(deviceMenu, &QMenu::aboutToShow, this, [this] {
        if (this->voice)
            VoiceActions::fillDeviceMenu(deviceMenu, this->voice, [this] { emit voiceSettingsRequested(); });
    });

    deafenButton = callButton(this);
    bar->addWidget(deafenButton);
    connect(deafenButton, &QToolButton::clicked, this, [this] {
        if (this->voice)
            this->voice->toggleSelfDeafen();
    });
    if (voice)
        connect(voice, &Core::Audio::VoiceManager::selfAudioChanged, this, &CallControlsBar::refreshVoiceButtons);

    cameraAvailable = camera && Core::Video::cameraCaptureAvailable();
    cameraButton = callButton(this);
    cameraButton->setPopupMode(QToolButton::MenuButtonPopup);
    bar->addWidget(cameraButton);
    connect(cameraButton, &QToolButton::clicked, this, &CallControlsBar::onCameraClicked);
    cameraMenu = new QMenu(cameraButton);
    cameraButton->setMenu(cameraMenu);
    connect(cameraMenu, &QMenu::aboutToShow, this, &CallControlsBar::fillCameraMenu);

    captureAvailable = broadcaster && (Core::Video::screenCaptureAvailable() || Core::Video::windowCaptureAvailable());
    shareButton = callButton(this);
    bar->addWidget(shareButton);
    connect(shareButton, &QToolButton::clicked, this, &CallControlsBar::onShareClicked);
    shareQualityMenu = new QMenu(shareButton);
    connect(shareQualityMenu, &QMenu::aboutToShow, this, [this] { fillShareQualityMenu(shareQualityMenu, this->broadcaster); });

    leaveButton = callButton(this);
    leaveButton->setIcon(Icons::icon(Icons::Name::PhoneOff, Token::ChatError));
    leaveButton->setToolTip(tr("Disconnect"));
    bar->addWidget(leaveButton);
    connect(leaveButton, &QToolButton::clicked, this, &CallControlsBar::leaveClicked);

    nonVideoCheck = new QCheckBox(tr("Show Non-Video Participants"), this);
    nonVideoCheck->setChecked(nonVideoShown);
    bar->addWidget(nonVideoCheck);
    connect(nonVideoCheck, &QCheckBox::toggled, this, &CallControlsBar::nonVideoShownChanged);

    othersButton = new QPushButton(tr("Hide Others"), this);
    othersButton->setCheckable(true);
    othersButton->setToolTip(tr("Give the focused video the whole view"));
    othersButton->hide();
    bar->addWidget(othersButton);
    connect(othersButton, &QPushButton::clicked, this, &CallControlsBar::othersHiddenChanged);

    statsButton = new QPushButton(tr("Stats"), this);
    statsButton->setCheckable(true);
    bar->addWidget(statsButton);
    connect(statsButton, &QPushButton::toggled, this, &CallControlsBar::statsShownChanged);

    popOutButton = new QPushButton(this);
    bar->addWidget(popOutButton);
    connect(popOutButton, &QPushButton::clicked, this, &CallControlsBar::popOutClicked);

    fullScreenButton = new QPushButton(this);
    bar->addWidget(fullScreenButton);
    connect(fullScreenButton, &QPushButton::clicked, this, &CallControlsBar::fullScreenClicked);
}

void CallControlsBar::setSummary(const QString &text)
{
    summaryLabel->setText(text);
}

void CallControlsBar::setOthersToggle(bool available, bool othersHidden)
{
    othersToggleAvailable = available;
    othersButton->setVisible(available && !barOnly);
    othersButton->setChecked(othersHidden);
    othersButton->setText(othersHidden ? tr("Show Others") : tr("Hide Others"));
}

bool CallControlsBar::nonVideoShown() const
{
    return nonVideoCheck->isChecked();
}

bool CallControlsBar::statsShown() const
{
    return statsButton->isChecked();
}

void CallControlsBar::apply(const Chrome &chrome)
{
    barOnly = !chrome.poppedOut && chrome.collapsed;
    setVisible(!chrome.fullScreen);
    collapseButton->setVisible(!chrome.poppedOut);
    collapseButton->setArrowType(chrome.collapsed ? Qt::RightArrow : Qt::DownArrow);

    refreshVoiceButtons();

    const bool sharing = broadcaster && broadcaster->isSharing();
    shareButton->setVisible(captureAvailable && broadcaster);
    shareButton->setEnabled(sharing || (broadcaster && broadcaster->canShare()));
    shareButton->setToolTip(sharing ? tr("Stop Sharing") : tr("Share Your Screen"));
    shareButton->setIcon(Icons::icon(sharing ? Icons::Name::ScreenShareOff : Icons::Name::ScreenShare, Token::ButtonText));
    shareButton->setPopupMode(sharing ? QToolButton::MenuButtonPopup : QToolButton::DelayedPopup);
    shareButton->setMenu(sharing ? shareQualityMenu : nullptr);

    const bool cameraOn = camera && camera->isOn();
    cameraButton->setVisible(cameraAvailable && camera);
    cameraButton->setEnabled(cameraOn || (camera && camera->canTurnOn()));
    cameraButton->setToolTip(cameraOn ? tr("Turn Off Camera") : tr("Turn On Camera"));
    cameraButton->setIcon(Icons::icon(cameraOn ? Icons::Name::VideoOff : Icons::Name::Video, Token::ButtonText));

    nonVideoCheck->setVisible(chrome.videoPresent && !barOnly);
    othersButton->setVisible(othersToggleAvailable && !barOnly);
    statsButton->setVisible(chrome.videoPresent && !barOnly);
    popOutButton->setVisible(chrome.videoPresent || chrome.poppedOut);
    fullScreenButton->setVisible(chrome.videoPresent || chrome.poppedOut);
    popOutButton->setText(chrome.poppedOut ? tr("Dock") : tr("Pop Out"));
    fullScreenButton->setText(chrome.fullScreen ? tr("Exit Full Screen") : tr("Full Screen"));
}

void CallControlsBar::refreshVoiceButtons()
{
    const bool muted = voice && voice->isSelfMuted();
    const bool deafened = voice && voice->isSelfDeafened();
    micButton->setIcon(Icons::icon(muted ? Icons::Name::MicOff : Icons::Name::Mic, muted ? Token::ChatError : Token::ButtonText));
    micButton->setToolTip(muted ? tr("Unmute") : tr("Mute"));
    deafenButton->setIcon(Icons::icon(deafened ? Icons::Name::HeadphoneOff : Icons::Name::Headphones, deafened ? Token::ChatError : Token::ButtonText));
    deafenButton->setToolTip(deafened ? tr("Undeafen") : tr("Deafen"));
}

void CallControlsBar::onShareClicked()
{
    if (!broadcaster)
        return;
    if (broadcaster->isSharing()) {
        broadcaster->stop();
        return;
    }

    auto *picker = new ScreenSharePicker(broadcaster->tier(), this);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    connect(picker, &ScreenSharePicker::shareRequested, this, [this](const Core::Video::CaptureSource &source, const SendQuality &quality, bool withAudio) {
        if (!broadcaster) {
            Core::Video::releaseSource(source);
            return;
        }
        Core::Video::restoreIfMinimised(source);
        broadcaster->start(source, quality, withAudio);
    });
    picker->open();
}

void CallControlsBar::onCameraClicked()
{
    if (!camera)
        return;
    if (camera->isOn())
        camera->turnOff();
    else
        camera->turnOn(Core::Video::preferredCamera(Core::Video::cameraDevices()));
}

void CallControlsBar::fillCameraMenu()
{
    cameraMenu->clear();

    const QList<Core::Video::CameraDevice> devices = Core::Video::cameraDevices();
    if (devices.isEmpty()) {
        cameraMenu->addAction(tr("No camera found"))->setEnabled(false);
        return;
    }

    const QString remembered = Core::Video::preferredCamera(devices);
    auto *choices = new QActionGroup(cameraMenu);
    for (const Core::Video::CameraDevice &device : devices) {
        QAction *action = cameraMenu->addAction(device.name);
        action->setCheckable(true);
        action->setChecked(remembered.isEmpty() ? device.id == devices.first().id : device.id == remembered);
        choices->addAction(action);
        connect(action, &QAction::triggered, this, [this, id = device.id] { chooseCamera(id); });
    }
}

void CallControlsBar::chooseCamera(const QString &deviceId)
{
    Core::Video::setPreferredCamera(deviceId);
    if (camera && camera->isOn())
        camera->turnOn(deviceId);
}

} // namespace UI
} // namespace Acheron
