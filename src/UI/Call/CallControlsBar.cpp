#include "UI/Call/CallControlsBar.hpp"

#include "Core/Streams/StreamBroadcaster.hpp"
#include "Core/Theme/Icons.hpp"
#include "Core/Video/CameraPipeline.hpp"
#include "Core/Video/CameraSender.hpp"
#include "Core/Video/ScreenCapture.hpp"
#include "UI/ElidedLabel.hpp"
#include "UI/ScreenSharePicker.hpp"

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

namespace {

const char *const SHARE_ICON = "screen-share";
const char *const STOP_SHARING_ICON = "screen-share-off";
const char *const CAMERA_ICON = "video";
const char *const STOP_CAMERA_ICON = "video-off";

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

CallControlsBar::CallControlsBar(StreamBroadcaster *broadcaster, CameraSender *camera, bool nonVideoShown, QWidget *parent)
    : QWidget(parent), broadcaster(broadcaster), camera(camera)
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

    captureAvailable = broadcaster && (Core::Video::screenCaptureAvailable() || Core::Video::windowCaptureAvailable());
    shareButton = new QToolButton(this);
    shareButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    bar->addWidget(shareButton);
    connect(shareButton, &QToolButton::clicked, this, &CallControlsBar::onShareClicked);
    shareQualityMenu = new QMenu(shareButton);
    connect(shareQualityMenu, &QMenu::aboutToShow, this, [this] { fillShareQualityMenu(shareQualityMenu, this->broadcaster); });

    cameraAvailable = camera && Core::Video::cameraCaptureAvailable();
    cameraButton = new QToolButton(this);
    cameraButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    cameraButton->setPopupMode(QToolButton::MenuButtonPopup);
    bar->addWidget(cameraButton);
    connect(cameraButton, &QToolButton::clicked, this, &CallControlsBar::onCameraClicked);
    cameraMenu = new QMenu(cameraButton);
    cameraButton->setMenu(cameraMenu);
    connect(cameraMenu, &QMenu::aboutToShow, this, &CallControlsBar::fillCameraMenu);

    nonVideoCheck = new QCheckBox(tr("Show Non-Video Participants"), this);
    nonVideoCheck->setChecked(nonVideoShown);
    bar->addWidget(nonVideoCheck);
    connect(nonVideoCheck, &QCheckBox::toggled, this, &CallControlsBar::nonVideoShownChanged);

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
    const bool barOnly = !chrome.poppedOut && chrome.collapsed;
    setVisible(!chrome.fullScreen);
    collapseButton->setVisible(!chrome.poppedOut);
    collapseButton->setArrowType(chrome.collapsed ? Qt::RightArrow : Qt::DownArrow);

    const bool sharing = broadcaster && broadcaster->isSharing();
    shareButton->setVisible(captureAvailable && broadcaster);
    shareButton->setEnabled(sharing || (broadcaster && broadcaster->canShare()));
    shareButton->setText(sharing ? tr("Stop Sharing") : tr("Share Screen"));
    shareButton->setIcon(Core::Theme::Icons::icon(QLatin1String(sharing ? STOP_SHARING_ICON : SHARE_ICON), Core::Theme::Token::ButtonText));
    shareButton->setPopupMode(sharing ? QToolButton::MenuButtonPopup : QToolButton::DelayedPopup);
    shareButton->setMenu(sharing ? shareQualityMenu : nullptr);

    const bool cameraOn = camera && camera->isOn();
    cameraButton->setVisible(cameraAvailable && camera);
    cameraButton->setEnabled(cameraOn || (camera && camera->canTurnOn()));
    cameraButton->setText(cameraOn ? tr("Stop Camera") : tr("Camera"));
    cameraButton->setToolTip(cameraOn ? tr("Turn Off Camera") : tr("Turn On Camera"));
    cameraButton->setIcon(Core::Theme::Icons::icon(QLatin1String(cameraOn ? STOP_CAMERA_ICON : CAMERA_ICON), Core::Theme::Token::ButtonText));

    nonVideoCheck->setVisible(chrome.videoPresent && !barOnly);
    statsButton->setVisible(chrome.videoPresent && !barOnly);
    popOutButton->setVisible(chrome.videoPresent || chrome.poppedOut);
    fullScreenButton->setVisible(chrome.videoPresent || chrome.poppedOut);
    popOutButton->setText(chrome.poppedOut ? tr("Dock") : tr("Pop Out"));
    fullScreenButton->setText(chrome.fullScreen ? tr("Exit Full Screen") : tr("Full Screen"));
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
