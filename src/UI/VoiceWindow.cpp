#include "VoiceWindow.hpp"

#include "Core/AV/VoiceManager.hpp"
#include "Core/ImageManager.hpp"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QFrame>

namespace Acheron {
namespace UI {

VolumeMeter::VolumeMeter(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(12);
}

void VolumeMeter::setLevel(float rms)
{
    level = rms;
    update();
}

void VolumeMeter::setThreshold(float t)
{
    threshold = t;
    update();
}

void VolumeMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    p.fillRect(rect(), palette().color(QPalette::Base));

    float fraction = qBound(0.0f, level / MAX_RMS, 1.0f);
    int barWidth = static_cast<int>(fraction * width());

    if (barWidth > 0) {
        QLinearGradient gradient(0, 0, width(), 0);
        gradient.setColorAt(0.0, QColor(0x43, 0xb5, 0x81));
        gradient.setColorAt(0.5, QColor(0xfa, 0xa6, 0x1a));
        gradient.setColorAt(1.0, QColor(0xf0, 0x47, 0x47));

        p.fillRect(0, 0, barWidth, height(), gradient);
    }

    if (threshold >= 0.0f) {
        float threshFraction = qBound(0.0f, threshold / MAX_RMS, 1.0f);
        int threshX = static_cast<int>(threshFraction * width());
        p.setPen(QPen(palette().color(QPalette::WindowText), 1));
        p.drawLine(threshX, 0, threshX, height());
    }
}

VoiceAvatar::VoiceAvatar(QWidget *parent) : QWidget(parent)
{
    setFixedSize(OUTER_SIZE, OUTER_SIZE);
}

void VoiceAvatar::setPixmap(const QPixmap &pm)
{
    avatar = pm;
    update();
}

void VoiceAvatar::setSpeaking(bool s)
{
    if (speaking == s)
        return;
    speaking = s;
    update();
}

void VoiceAvatar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int s = qMin(width(), height());

    if (speaking) {
        p.setPen(QPen(palette().color(QPalette::Highlight), BORDER_WIDTH));
        p.setBrush(Qt::NoBrush);
        qreal half = BORDER_WIDTH / 2.0;
        p.drawRoundedRect(QRectF(half, half, s - BORDER_WIDTH, s - BORDER_WIDTH),
                          AVATAR_RADIUS + 1, AVATAR_RADIUS + 1);
    }

    int inset = BORDER_WIDTH + 1;
    QRectF avatarRect(inset, inset, s - inset * 2, s - inset * 2);

    QPainterPath clip;
    clip.addRoundedRect(avatarRect, AVATAR_RADIUS, AVATAR_RADIUS);
    p.setClipPath(clip);

    if (!avatar.isNull())
        p.drawPixmap(avatarRect.toRect(), avatar);
    else {
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Mid));
        p.drawRoundedRect(avatarRect, AVATAR_RADIUS, AVATAR_RADIUS);
    }
}

VoiceUserWidget::VoiceUserWidget(Core::Snowflake userId, QWidget *parent)
    : QWidget(parent), userId(userId)
{
    setFixedHeight(36);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(4, 2, 4, 2);
    row->setSpacing(4);

    avatarWidget = new VoiceAvatar(this);

    nameLabel = new QLabel(this);
    nameLabel->setMinimumWidth(40);
    nameLabel->setTextFormat(Qt::PlainText);

    meter = new VolumeMeter(this);
    meter->setFixedWidth(40);
    meter->setFixedHeight(8);

    volumeSlider = new QSlider(Qt::Horizontal, this);
    volumeSlider->setRange(0, 200);
    volumeSlider->setValue(100);
    volumeSlider->setMinimumWidth(40);

    muteBtn = new QPushButton(tr("Mute"), this);
    muteBtn->setFixedHeight(20);
    muteBtn->setCheckable(true);
    muteBtn->setStyleSheet(
            "QPushButton { background-color: transparent; border: 1px solid palette(dark);"
            "  border-radius: 3px; font-size: 10px; padding: 0 4px; }"
            "QPushButton:checked { background-color: #f04747; color: #fff; border-color: #f04747; }"
            "QPushButton:hover { border-color: palette(highlight); }");

    stateLabel = new QLabel(this);
    stateLabel->setFixedWidth(24);
    stateLabel->setAlignment(Qt::AlignCenter);
    stateLabel->setStyleSheet("QLabel { color: palette(mid); font-size: 9px; }");

    row->addWidget(avatarWidget);
    row->addWidget(nameLabel, 1);
    row->addWidget(meter);
    row->addWidget(volumeSlider, 1);
    row->addWidget(muteBtn);
    row->addWidget(stateLabel);

    speakingDecayTimer.setSingleShot(true);
    speakingDecayTimer.setInterval(SPEAKING_DECAY_MS);
    connect(&speakingDecayTimer, &QTimer::timeout, this, [this]() {
        avatarWidget->setSpeaking(false);
    });

    connect(volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        if (locallyMuted)
            return;
        emit volumeChanged(this->userId, value);
    });

    connect(muteBtn, &QPushButton::toggled, this, [this](bool checked) {
        locallyMuted = checked;
        emit muteToggled(this->userId, checked);
    });
}

void VoiceUserWidget::setSpeaking(bool speaking)
{
    if (speaking) {
        avatarWidget->setSpeaking(true);
        speakingDecayTimer.start();
    } else {
        speakingDecayTimer.stop();
        avatarWidget->setSpeaking(false);
    }
}

void VoiceUserWidget::setAudioLevel(float rms)
{
    meter->setLevel(rms);

    if (rms > 0.0f) {
        avatarWidget->setSpeaking(true);
        speakingDecayTimer.start();
    }
}

void VoiceUserWidget::setVoiceStateFlags(bool selfMute, bool selfDeaf, bool serverMute, bool serverDeaf, bool suppress)
{
    QString flags;
    if (serverDeaf || selfDeaf)
        flags += "D";
    if (serverMute || selfMute)
        flags += "M";
    if (suppress)
        flags += "S";
    stateLabel->setText(flags);
}

void VoiceUserWidget::setLocallyMuted(bool muted)
{
    locallyMuted = muted;
    muteBtn->setChecked(muted);
}

void VoiceUserWidget::setVolume(int pct)
{
    volumeSlider->blockSignals(true);
    volumeSlider->setValue(pct);
    volumeSlider->blockSignals(false);
}

void VoiceUserWidget::contextMenuEvent(QContextMenuEvent *event)
{
    if (!daveActive)
        return;

    QMenu menu(this);

    auto *verifyAction = menu.addAction(tr("View Verification Code"));
    connect(verifyAction, &QAction::triggered, this, [this]() {
        emit verificationCodeRequested(userId);
    });

    menu.exec(event->globalPos());
}

void VoiceUserWidget::setDaveActive(bool active)
{
    daveActive = active;
}

void VoiceUserWidget::setDisplayName(const QString &name)
{
    nameLabel->setText(name);
}

void VoiceUserWidget::setAvatar(const QPixmap &pm)
{
    avatarWidget->setPixmap(pm);
}

VoiceWindow::VoiceWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Voice Settings"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setupUi();
}

void VoiceWindow::setupUi()
{
    setMinimumWidth(280);
    resize(320, 0);

    setStyleSheet("QLabel { font-size: 11px; background: transparent; }"
                  "QSlider::groove:horizontal { background: palette(dark); height: 4px; border-radius: 2px; }"
                  "QSlider::handle:horizontal { background: palette(highlight); width: 12px; margin: -4px 0; border-radius: 6px; }"
                  "QSlider::sub-page:horizontal { background: palette(highlight); border-radius: 2px; }"
                  "QComboBox { background: palette(base); border: 1px solid palette(mid); border-radius: 3px; padding: 2px 6px; font-size: 11px; }"
                  "QComboBox::drop-down { border: none; }"
                  "QComboBox QAbstractItemView { background: palette(base); selection-background-color: palette(highlight); border: 1px solid palette(mid); }");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(6);

    auto *usersHeaderRow = new QHBoxLayout;
    usersHeaderRow->setContentsMargins(0, 0, 0, 0);
    auto *usersHeader = new QLabel(tr("Connected Users"), this);
    usersHeader->setStyleSheet("QLabel { font-size: 10px; font-weight: bold; text-transform: uppercase; }");
    usersHeaderRow->addWidget(usersHeader);
    usersHeaderRow->addStretch();
    privacyCodeBtn = new QPushButton(tr("Privacy Code"), this);
    privacyCodeBtn->setFlat(true);
    privacyCodeBtn->setCursor(Qt::PointingHandCursor);
    privacyCodeBtn->hide();
    privacyCodeBtn->setStyleSheet(
            "QPushButton { font-size: 10px; color: palette(highlight); background: transparent;"
            "  border: none; padding: 0; text-transform: uppercase; font-weight: bold; }"
            "QPushButton:hover { text-decoration: underline; }");
    connect(privacyCodeBtn, &QPushButton::clicked, this, &VoiceWindow::showPrivacyCode);
    usersHeaderRow->addWidget(privacyCodeBtn);
    layout->addLayout(usersHeaderRow);

    userScrollArea = new QScrollArea(this);
    userScrollArea->setWidgetResizable(true);
    userScrollArea->setFrameShape(QFrame::NoFrame);
    userScrollArea->setStyleSheet("QScrollArea { background: transparent; }"
                                  "QScrollBar:vertical { background: palette(window); width: 6px; }"
                                  "QScrollBar::handle:vertical { background: palette(mid); border-radius: 3px; }"
                                  "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");
    userScrollArea->setMinimumHeight(40);

    userListContainer = new QWidget;
    userListLayout = new QVBoxLayout(userListContainer);
    userListLayout->setContentsMargins(0, 0, 0, 0);
    userListLayout->setSpacing(0);
    userListLayout->addStretch();
    userScrollArea->setWidget(userListContainer);
    layout->addWidget(userScrollArea, 1);

    auto *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setStyleSheet("QFrame { color: palette(mid); }");
    layout->addWidget(separator);

    volumeMeter = new VolumeMeter(this);
    volumeMeter->setThreshold(100.0f);
    layout->addWidget(volumeMeter);

    auto *inputDevRow = new QHBoxLayout;
    inputDevRow->setSpacing(4);
    auto *inputDevLabel = new QLabel(tr("Input:"), this);
    inputDevLabel->setFixedWidth(44);
    inputDeviceCombo = new QComboBox(this);
    inputDeviceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    inputDevRow->addWidget(inputDevLabel);
    inputDevRow->addWidget(inputDeviceCombo, 1);
    layout->addLayout(inputDevRow);

    auto *outputDevRow = new QHBoxLayout;
    outputDevRow->setSpacing(4);
    auto *outputDevLabel = new QLabel(tr("Output:"), this);
    outputDevLabel->setFixedWidth(44);
    outputDeviceCombo = new QComboBox(this);
    outputDeviceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    outputDevRow->addWidget(outputDevLabel);
    outputDevRow->addWidget(outputDeviceCombo, 1);
    layout->addLayout(outputDevRow);

    auto *inputRow = new QHBoxLayout;
    inputRow->setSpacing(4);
    auto *inputLabel = new QLabel(tr("Input"), this);
    inputLabel->setFixedWidth(44);
    inputGainSlider = new QSlider(Qt::Horizontal, this);
    inputGainSlider->setRange(0, 200);
    inputGainSlider->setValue(100);
    inputGainValue = new QLabel("100%", this);
    inputGainValue->setFixedWidth(36);
    inputGainValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    inputRow->addWidget(inputLabel);
    inputRow->addWidget(inputGainSlider, 1);
    inputRow->addWidget(inputGainValue);
    layout->addLayout(inputRow);

    auto *outputRow = new QHBoxLayout;
    outputRow->setSpacing(4);
    auto *outputLabel = new QLabel(tr("Output"), this);
    outputLabel->setFixedWidth(44);
    outputVolumeSlider = new QSlider(Qt::Horizontal, this);
    outputVolumeSlider->setRange(0, 200);
    outputVolumeSlider->setValue(100);
    outputVolumeValue = new QLabel("100%", this);
    outputVolumeValue->setFixedWidth(36);
    outputVolumeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    outputRow->addWidget(outputLabel);
    outputRow->addWidget(outputVolumeSlider, 1);
    outputRow->addWidget(outputVolumeValue);
    layout->addLayout(outputRow);

    auto *vadRow = new QHBoxLayout;
    vadRow->setSpacing(4);
    auto *vadLabel = new QLabel(tr("VAD"), this);
    vadLabel->setFixedWidth(44);
    vadThresholdSlider = new QSlider(Qt::Horizontal, this);
    vadThresholdSlider->setRange(0, 2000);
    vadThresholdSlider->setValue(100);
    vadThresholdValue = new QLabel("100", this);
    vadThresholdValue->setFixedWidth(36);
    vadThresholdValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    vadRow->addWidget(vadLabel);
    vadRow->addWidget(vadThresholdSlider, 1);
    vadRow->addWidget(vadThresholdValue);
    layout->addLayout(vadRow);

    connect(inputGainSlider, &QSlider::valueChanged, this, [this](int value) {
        inputGainValue->setText(QString("%1%").arg(value));
        if (voiceManager)
            voiceManager->setInputGain(static_cast<float>(value) / 100.0f);
    });

    connect(outputVolumeSlider, &QSlider::valueChanged, this, [this](int value) {
        outputVolumeValue->setText(QString("%1%").arg(value));
        if (voiceManager)
            voiceManager->setOutputVolume(static_cast<float>(value) / 100.0f);
    });

    connect(vadThresholdSlider, &QSlider::valueChanged, this, [this](int value) {
        vadThresholdValue->setText(QString::number(value));
        volumeMeter->setThreshold(static_cast<float>(value));
        if (voiceManager)
            voiceManager->setVadThreshold(static_cast<float>(value));
    });

    connect(inputDeviceCombo, &QComboBox::activated, this, [this](int index) {
        if (!voiceManager)
            return;
        QByteArray deviceId = inputDeviceCombo->itemData(index).toByteArray();
        voiceManager->setInputDevice(deviceId);
    });

    connect(outputDeviceCombo, &QComboBox::activated, this, [this](int index) {
        if (!voiceManager)
            return;
        QByteArray deviceId = outputDeviceCombo->itemData(index).toByteArray();
        voiceManager->setOutputDevice(deviceId);
    });
}

void VoiceWindow::setVoiceManager(Core::AV::VoiceManager *manager)
{
    if (voiceManager == manager)
        return;

    disconnectManager();
    voiceManager = manager;

    if (!voiceManager)
        return;

    connect(voiceManager, &Core::AV::VoiceManager::audioLevelChanged, volumeMeter, &VolumeMeter::setLevel);
    connect(voiceManager, &Core::AV::VoiceManager::devicesChanged, this, &VoiceWindow::refreshDevices);
    connect(voiceManager, &Core::AV::VoiceManager::participantJoined, this, &VoiceWindow::onParticipantJoined);
    connect(voiceManager, &Core::AV::VoiceManager::participantLeft, this, &VoiceWindow::onParticipantLeft);
    connect(voiceManager, &Core::AV::VoiceManager::participantUpdated, this, &VoiceWindow::onParticipantUpdated);
    connect(voiceManager, &Core::AV::VoiceManager::participantSpeakingChanged, this, &VoiceWindow::onParticipantSpeakingChanged);
    connect(voiceManager, &Core::AV::VoiceManager::userAudioLevelChanged, this, &VoiceWindow::onUserAudioLevelChanged);
    connect(voiceManager, &Core::AV::VoiceManager::participantsCleared, this, &VoiceWindow::onParticipantsCleared);
    connect(voiceManager, &Core::AV::VoiceManager::privacyCodeChanged,
            this, [this](const QString &code) {
                bool active = !code.isEmpty();
                privacyCodeBtn->setVisible(active);
                for (auto *w : userWidgets)
                    w->setDaveActive(active);
            });

    privacyCodeBtn->setVisible(!voiceManager->privacyCode().isEmpty());

    const auto existing = voiceManager->currentParticipants();
    for (const auto &p : existing)
        onParticipantJoined(p.userId);

    refreshDevices();
}

void VoiceWindow::setNameResolver(NameResolver resolver)
{
    nameResolver = std::move(resolver);

    for (auto it = userWidgets.constBegin(); it != userWidgets.constEnd(); ++it) {
        QString name = nameResolver ? nameResolver(it.key()) : QString::number(it.key());
        it.value()->setDisplayName(name);
    }
}

void VoiceWindow::setAvatarResolver(AvatarResolver resolver)
{
    avatarResolver = std::move(resolver);

    for (auto it = userWidgets.begin(); it != userWidgets.end(); ++it)
        requestAvatar(it.key(), it.value());
}

void VoiceWindow::setImageManager(Core::ImageManager *manager)
{
    if (imageManager == manager)
        return;

    if (imageFetchedConn)
        QObject::disconnect(imageFetchedConn);

    imageManager = manager;
    pendingAvatars.clear();

    if (imageManager) {
        imageFetchedConn = connect(imageManager, &Core::ImageManager::imageFetched,
                                   this, &VoiceWindow::onImageFetched);
    }
}

void VoiceWindow::refreshDevices()
{
    if (!voiceManager)
        return;

    auto populateCombo = [](QComboBox *combo, const QList<Core::AV::AudioDeviceInfo> &devices,
                            const QByteArray &currentId) {
        combo->blockSignals(true);
        combo->clear();
        int selectedIndex = 0;
        for (int i = 0; i < devices.size(); i++) {
            const auto &dev = devices[i];
            QString label = dev.description;
            if (dev.isDefault)
                label += QObject::tr(" (Default)");
            combo->addItem(label, dev.id);
            if (!currentId.isEmpty() && dev.id == currentId)
                selectedIndex = i;
            else if (currentId.isEmpty() && dev.isDefault)
                selectedIndex = i;
        }
        combo->setCurrentIndex(selectedIndex);
        combo->blockSignals(false);
    };

    populateCombo(inputDeviceCombo, voiceManager->availableInputDevices(), voiceManager->currentInputDevice());
    populateCombo(outputDeviceCombo, voiceManager->availableOutputDevices(), voiceManager->currentOutputDevice());
}

void VoiceWindow::disconnectManager()
{
    if (voiceManager) {
        disconnect(voiceManager, nullptr, this, nullptr);
        disconnect(voiceManager, nullptr, volumeMeter, nullptr);
    }
    voiceManager = nullptr;

    qDeleteAll(userWidgets);
    userWidgets.clear();
    pendingAvatars.clear();
    privacyCodeBtn->hide();
}

void VoiceWindow::onParticipantJoined(Core::Snowflake userId)
{
    if (userWidgets.contains(userId))
        return;

    auto *widget = new VoiceUserWidget(userId, userListContainer);

    QString name = nameResolver ? nameResolver(userId) : QString::number(userId);
    widget->setDisplayName(name);

    if (voiceManager) {
        widget->setLocallyMuted(voiceManager->isUserMuted(userId));
        widget->setDaveActive(!voiceManager->privacyCode().isEmpty());

        const auto *p = voiceManager->participant(userId);
        if (p)
            widget->setVoiceStateFlags(p->selfMute, p->selfDeaf, p->serverMute, p->serverDeaf, p->suppress);
    }

    connect(widget, &VoiceUserWidget::volumeChanged, this, [this](Core::Snowflake uid, int pct) {
        if (voiceManager)
            voiceManager->setUserVolume(uid, static_cast<float>(pct) / 100.0f);
    });

    connect(widget, &VoiceUserWidget::muteToggled, this, [this](Core::Snowflake uid, bool muted) {
        if (!voiceManager)
            return;
        voiceManager->setUserMuted(uid, muted);

        if (!muted) {
            auto it = userWidgets.constFind(uid);
            if (it != userWidgets.constEnd())
                voiceManager->setUserVolume(uid, static_cast<float>((*it)->volume()) / 100.0f);
        }
    });

    connect(widget, &VoiceUserWidget::verificationCodeRequested,
            this, &VoiceWindow::showVerificationCode);

    userListLayout->insertWidget(userListLayout->count() - 1, widget);
    userWidgets.insert(userId, widget);

    requestAvatar(userId, widget);
}

void VoiceWindow::onParticipantLeft(Core::Snowflake userId)
{
    auto it = userWidgets.find(userId);
    if (it == userWidgets.end())
        return;

    delete it.value();
    userWidgets.erase(it);
}

void VoiceWindow::onParticipantUpdated(Core::Snowflake userId)
{
    auto it = userWidgets.constFind(userId);
    if (it == userWidgets.constEnd())
        return;

    if (!voiceManager)
        return;

    const auto *p = voiceManager->participant(userId);
    if (p)
        it.value()->setVoiceStateFlags(p->selfMute, p->selfDeaf, p->serverMute, p->serverDeaf, p->suppress);
}

void VoiceWindow::onParticipantSpeakingChanged(Core::Snowflake userId, bool speaking)
{
    auto it = userWidgets.constFind(userId);
    if (it != userWidgets.constEnd())
        it.value()->setSpeaking(speaking);
}

void VoiceWindow::onUserAudioLevelChanged(Core::Snowflake userId, float rms)
{
    auto it = userWidgets.constFind(userId);
    if (it != userWidgets.constEnd())
        it.value()->setAudioLevel(rms);
}

void VoiceWindow::onParticipantsCleared()
{
    qDeleteAll(userWidgets);
    userWidgets.clear();
    pendingAvatars.clear();
}

void VoiceWindow::onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap)
{
    if (size != AVATAR_REQUEST_SIZE)
        return;

    auto it = pendingAvatars.find(url);
    if (it == pendingAvatars.end())
        return;

    Core::Snowflake userId = it.value();
    pendingAvatars.erase(it);

    auto widgetIt = userWidgets.constFind(userId);
    if (widgetIt != userWidgets.constEnd())
        widgetIt.value()->setAvatar(pixmap);
}

static QDialog *createCodeDialog(QWidget *parent, const QString &title,
                                 const QString &description, const QString &code)
{
    auto *dlg = new QDialog(parent);
    dlg->setWindowTitle(title);
    dlg->setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(dlg);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 16, 20, 16);

    auto *desc = new QLabel(description, dlg);
    desc->setWordWrap(true);
    layout->addWidget(desc);

    auto *codeLabel = new QLabel(code, dlg);
    codeLabel->setAlignment(Qt::AlignCenter);
    codeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    codeFont.setPointSize(codeFont.pointSize() + 4);
    codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 1);
    codeLabel->setFont(codeFont);
    codeLabel->setStyleSheet(
            "QLabel { background: palette(base); border: 1px solid palette(mid);"
            "  border-radius: 6px; padding: 12px 16px; }");
    layout->addWidget(codeLabel);

    auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    QObject::connect(btnBox, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    layout->addWidget(btnBox);

    dlg->setMinimumWidth(340);
    return dlg;
}

void VoiceWindow::showVerificationCode(Core::Snowflake userId)
{
    if (!voiceManager) {
        QMessageBox::information(this, tr("Verification Code"), tr("Not connected to voice."));
        return;
    }

    if (!voiceManager->isDaveEnabled()) {
        QMessageBox::information(this, tr("Verification Code"),
                                 tr("End-to-end encryption is not active in this call."));
        return;
    }

    QString userName = nameResolver ? nameResolver(userId) : QString::number(userId);
    QPointer<VoiceWindow> self = this;

    voiceManager->requestVerificationCode(userId, [self, userName](const QString &code) {
        // callback triggered from dave created thread
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, userName, code]() {
            if (!self)
                return;

            if (code.isEmpty()) {
                QMessageBox::warning(self, VoiceWindow::tr("Verification Code"),
                                     VoiceWindow::tr("Could not generate verification code for %1.\n"
                                                     "They may not be in the MLS group yet.")
                                             .arg(userName));
                return;
            }

            auto *dlg = createCodeDialog(
                    self, VoiceWindow::tr("Verification Code"),
                    VoiceWindow::tr("Compare this code with <b>%1</b> to verify end-to-end encryption. "
                                    "Both sides should see the same code.")
                            .arg(userName.toHtmlEscaped()),
                    code);
            dlg->show();
        });
    });
}

void VoiceWindow::showPrivacyCode()
{
    if (!voiceManager) {
        QMessageBox::information(this, tr("Privacy Code"), tr("Not connected to voice."));
        return;
    }

    if (!voiceManager->isDaveEnabled()) {
        QMessageBox::information(this, tr("Privacy Code"),
                                 tr("End-to-end encryption is not active in this call."));
        return;
    }

    QString code = voiceManager->privacyCode();
    if (code.isEmpty()) {
        QMessageBox::warning(this, tr("Privacy Code"),
                             tr("Privacy code is not yet available."));
        return;
    }

    auto *dlg = createCodeDialog(
            this, tr("Privacy Code"),
            tr("This code is shared by everyone in this call. "
               "If everyone sees the same code, the call is end-to-end encrypted."),
            code);
    dlg->show();
}

void VoiceWindow::requestAvatar(Core::Snowflake userId, VoiceUserWidget *widget)
{
    if (!imageManager || !avatarResolver)
        return;

    QUrl url = avatarResolver(userId);
    if (!url.isValid())
        return;

    QPixmap pm = imageManager->get(url, AVATAR_REQUEST_SIZE);
    widget->setAvatar(pm);

    if (!imageManager->isCached(url, AVATAR_REQUEST_SIZE))
        pendingAvatars.insert(url, userId);
}

} // namespace UI
} // namespace Acheron
