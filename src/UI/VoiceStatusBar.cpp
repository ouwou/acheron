#include "VoiceStatusBar.hpp"
#include "VoiceWindow.hpp"

#include "Core/AV/VoiceManager.hpp"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>

namespace Acheron {
namespace UI {

VoiceStatusBar::VoiceStatusBar(QWidget *parent) : QWidget(parent)
{
    setupUi();
    hide();

    closeTimer.setSingleShot(true);
    closeTimer.setInterval(500);
    connect(&closeTimer, &QTimer::timeout, this, [this]() {
        if (voiceWindow)
            voiceWindow->close();
    });
}

void VoiceStatusBar::setupUi()
{
    setFixedHeight(32);
    setCursor(Qt::PointingHandCursor);

    setStyleSheet("QWidget { background-color: #1a1a2e; }"
                  "QLabel { background: transparent; }");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 0, 6, 0);
    layout->setSpacing(6);

    statusDot = new QLabel(this);
    statusDot->setFixedSize(8, 8);
    statusDot->setStyleSheet("QLabel { background-color: #999; border-radius: 4px; }");

    statusLabel = new QLabel(this);
    statusLabel->setStyleSheet("QLabel { color: #999; font-size: 11px; font-weight: bold; }");

    channelLabel = new QLabel(this);
    channelLabel->setStyleSheet("QLabel { color: #72767d; font-size: 11px; }");

    // idk why i cant just set some text so this will have to do ig
    auto makeXIcon = [](QColor color) {
        QPixmap px(16, 16);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(color, 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(4, 4, 12, 12);
        p.drawLine(12, 4, 4, 12);
        return QIcon(px);
    };

    disconnectBtn = new QPushButton(this);
    disconnectBtn->setFixedSize(24, 24);
    disconnectBtn->setIcon(makeXIcon(QColor(0xf0, 0x47, 0x47)));
    disconnectBtn->setIconSize(QSize(16, 16));
    disconnectBtn->setStyleSheet(
            "QPushButton { background: transparent; border: none; }"
            "QPushButton:hover { background: rgba(240, 71, 71, 30); border-radius: 4px; }");
    disconnectBtn->setCursor(Qt::PointingHandCursor);
    connect(disconnectBtn, &QPushButton::clicked, this, &VoiceStatusBar::disconnectRequested);

    layout->addWidget(statusDot);
    layout->addWidget(statusLabel);
    layout->addWidget(channelLabel);
    layout->addStretch();
    layout->addWidget(disconnectBtn);
}

void VoiceStatusBar::setVoiceManager(Core::AV::VoiceManager *manager)
{
    if (voiceManager == manager)
        return;

    disconnectManager();
    voiceManager = manager;

    if (!voiceManager) {
        if (voiceWindow)
            voiceWindow->setVoiceManager(nullptr);
        updateConnectionState();
        return;
    }

    connect(voiceManager, &Core::AV::VoiceManager::voiceStateChanged,
            this, &VoiceStatusBar::updateConnectionState);

    configureVoiceWindow();
    updateConnectionState();
}

void VoiceStatusBar::setNameResolver(NameResolver resolver)
{
    nameResolver = std::move(resolver);
    if (voiceWindow)
        voiceWindow->setNameResolver(nameResolver);
}

void VoiceStatusBar::setAvatarResolver(AvatarResolver resolver)
{
    avatarResolver = std::move(resolver);
    if (voiceWindow)
        voiceWindow->setAvatarResolver(avatarResolver);
}

void VoiceStatusBar::setImageManager(Core::ImageManager *manager)
{
    imageManager = manager;
    if (voiceWindow)
        voiceWindow->setImageManager(manager);
}

void VoiceStatusBar::setChannelName(const QString &name)
{
    if (name.isEmpty())
        channelLabel->hide();
    else {
        channelLabel->setText("#" + name);
        channelLabel->show();
    }
}

void VoiceStatusBar::updateConnectionState()
{
    using State = Discord::AV::VoiceClient::State;

    State state = State::Disconnected;
    if (voiceManager)
        state = voiceManager->clientState();

    bool connected = (state == State::Connected);
    bool connecting = (state == State::Connecting || state == State::Identifying ||
                       state == State::WaitingForReady || state == State::DiscoveringIP ||
                       state == State::SelectingProtocol || state == State::WaitingForSession);
    bool active = connected || connecting;

    QString dotColor;
    QString textColor;
    QString stateText;

    if (connected) {
        stateText = tr("Voice Connected");
        dotColor = "#43b581";
        textColor = "#43b581";
    } else if (connecting) {
        if (state == State::Connecting)
            stateText = tr("Connecting...");
        else if (state == State::Identifying)
            stateText = tr("Identifying...");
        else
            stateText = tr("Securing...");
        dotColor = "#faa61a";
        textColor = "#faa61a";
    } else {
        stateText = tr("Disconnected");
        dotColor = "#999";
        textColor = "#999";
    }

    statusDot->setStyleSheet(QStringLiteral("QLabel { background-color: %1; border-radius: 4px; }").arg(dotColor));
    statusLabel->setText(stateText);
    statusLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: bold; }").arg(textColor));

    setVisible(active);

    if (active && wasDisconnected) {
        closeTimer.stop();
        showVoiceWindow();
        if (voiceWindow)
            voiceWindow->refreshDevices();
    } else if (active) {
        closeTimer.stop();
    }

    if (!active && voiceWindow)
        closeTimer.start();

    wasDisconnected = !active;
}

void VoiceStatusBar::mousePressEvent(QMouseEvent *event)
{
    if (disconnectBtn->geometry().contains(event->pos())) {
        QWidget::mousePressEvent(event);
        return;
    }

    toggleVoiceWindow();
}

void VoiceStatusBar::toggleVoiceWindow()
{
    if (voiceWindow && voiceWindow->isVisible())
        voiceWindow->close();
    else
        showVoiceWindow();
}

void VoiceStatusBar::showVoiceWindow()
{
    if (!voiceWindow) {
        voiceWindow = new VoiceWindow(window());
        configureVoiceWindow();
    }

    voiceWindow->show();
    voiceWindow->raise();
    voiceWindow->activateWindow();
}

void VoiceStatusBar::configureVoiceWindow()
{
    if (!voiceWindow)
        return;
    if (imageManager)
        voiceWindow->setImageManager(imageManager);
    if (nameResolver)
        voiceWindow->setNameResolver(nameResolver);
    if (avatarResolver)
        voiceWindow->setAvatarResolver(avatarResolver);
    if (voiceManager)
        voiceWindow->setVoiceManager(voiceManager);
}

void VoiceStatusBar::disconnectManager()
{
    if (voiceManager)
        disconnect(voiceManager, nullptr, this, nullptr);
    voiceManager = nullptr;
}

} // namespace UI
} // namespace Acheron
