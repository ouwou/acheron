#include "VoiceActions.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Audio/VoiceSettings.hpp"
#include "UI/VolumeMeter.hpp"

#include <QActionGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace Acheron {
namespace UI {

using Core::Audio::AudioDeviceInfo;
using Core::Audio::VoiceManager;
using Core::Audio::VoiceSettings;

namespace {

constexpr int VOLUME_SLIDER_WIDTH = 120;

void addRow(QMenu *menu, QWidget *row)
{
    auto *action = new QWidgetAction(menu);
    action->setDefaultWidget(row);
    menu->addAction(action);
}

void addDeviceMenu(QMenu *menu, const QString &title, const QList<AudioDeviceInfo> &devices, const QByteArray &current, QByteArray VoiceSettings::*device)
{
    QMenu *choices = menu->addMenu(title);
    auto *group = new QActionGroup(choices);
    const auto addChoice = [choices, group, &current, device](const QString &label, const QByteArray &id) {
        QAction *action = choices->addAction(label);
        action->setCheckable(true);
        action->setChecked(id == current);
        group->addAction(action);
        QObject::connect(action, &QAction::triggered, action, [device, id] {
            VoiceSettings chosen = VoiceSettings::load();
            chosen.*device = id;
            chosen.saveAndApply();
        });
    };
    addChoice(VoiceActions::tr("Default"), {});
    for (const AudioDeviceInfo &each : devices)
        addChoice(each.description, each.id);
}

QDialog *codeDialog(QWidget *parent, const QString &title, const QString &description, const QString &code)
{
    auto *dialog = new QDialog(parent);
    dialog->setWindowTitle(title);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(dialog);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 16, 20, 16);

    auto *text = new QLabel(description, dialog);
    text->setWordWrap(true);
    layout->addWidget(text);

    auto *codeLabel = new QLabel(code, dialog);
    codeLabel->setAlignment(Qt::AlignCenter);
    codeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    codeFont.setPointSize(codeFont.pointSize() + 4);
    codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 1);
    codeLabel->setFont(codeFont);
    codeLabel->setStyleSheet("QLabel { background: palette(base); border: 1px solid palette(mid); border-radius: 6px; padding: 12px 16px; }");
    layout->addWidget(codeLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);

    dialog->setMinimumWidth(340);
    return dialog;
}

} // namespace

void VoiceActions::addVolumeSlider(QMenu *menu, const QString &label, int percent, const std::function<void(int)> &changed)
{
    auto *row = new QWidget(menu);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 2, 8, 2);
    layout->addWidget(new QLabel(label, row));
    auto *slider = new QSlider(Qt::Horizontal, row);
    slider->setRange(0, VoiceSettings::MAX_GAIN_PERCENT);
    slider->setValue(percent);
    slider->setFixedWidth(VOLUME_SLIDER_WIDTH);
    layout->addWidget(slider);
    auto *value = new QLabel(QStringLiteral("%1%").arg(percent), row);
    layout->addWidget(value);
    QObject::connect(slider, &QSlider::valueChanged, slider, [value, changed](int chosen) {
        value->setText(QStringLiteral("%1%").arg(chosen));
        changed(chosen);
    });
    addRow(menu, row);
}

void VoiceActions::addUserActions(QMenu *menu, VoiceManager *voice, Core::Snowflake userId, const QString &name)
{
    const QPointer<VoiceManager> guarded(voice);
    addVolumeSlider(menu, tr("User Volume"), voice->userVolume(userId), [guarded, userId](int percent) {
        if (guarded)
            guarded->setUserVolume(userId, percent);
    });

    QAction *mute = menu->addAction(tr("Mute"));
    mute->setCheckable(true);
    mute->setChecked(voice->isUserMuted(userId));
    QObject::connect(mute, &QAction::toggled, voice, [voice, userId](bool muted) { voice->setUserMuted(userId, muted); });

    if (voice->isDaveEnabled()) {
        QObject::connect(menu->addAction(tr("View Verification Code")), &QAction::triggered, voice, [voice, userId, name, parent = QPointer<QWidget>(menu->parentWidget())] {
            showVerificationCode(parent, voice, userId, name);
        });
    }
}

void VoiceActions::fillDeviceMenu(QMenu *menu, VoiceManager *voice, const std::function<void()> &openVoiceSettings)
{
    qDeleteAll(menu->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly));
    menu->clear();
    addDeviceMenu(menu, tr("Input Device"), voice->availableInputDevices(), voice->currentInputDevice(), &VoiceSettings::inputDevice);
    addDeviceMenu(menu, tr("Output Device"), voice->availableOutputDevices(), voice->currentOutputDevice(), &VoiceSettings::outputDevice);
    menu->addSeparator();

    auto *meterRow = new QWidget(menu);
    auto *layout = new QHBoxLayout(meterRow);
    layout->setContentsMargins(8, 4, 8, 4);
    auto *meter = new VolumeMeter(meterRow);
    meter->setToolTip(tr("Your microphone, and the level it has to reach to send"));
    meter->setThreshold(static_cast<float>(VoiceSettings::load().vadThreshold));
    layout->addWidget(meter);
    QObject::connect(voice, &VoiceManager::audioLevelChanged, meter, &VolumeMeter::setLevel);
    addRow(menu, meterRow);

    QObject::connect(menu->addAction(tr("Voice Settings...")), &QAction::triggered, menu, openVoiceSettings);
}

void VoiceActions::showPrivacyCode(QWidget *parent, VoiceManager *voice)
{
    const QString code = voice->isDaveEnabled() ? voice->privacyCode() : QString();
    if (code.isEmpty()) {
        QMessageBox::information(parent, tr("Privacy Code"), tr("End-to-end encryption is not active in this call yet."));
        return;
    }
    codeDialog(parent, tr("Privacy Code"), tr("This code is shared by everyone in this call. If everyone sees the same code, the call is end-to-end encrypted."), code)->show();
}

void VoiceActions::showVerificationCode(QWidget *parent, VoiceManager *voice, Core::Snowflake userId, const QString &name)
{
    const QPointer<QWidget> guardedParent(parent);
    voice->requestVerificationCode(userId, [guardedParent, name](const QString &code) {
        // called on libdave's thread
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guardedParent, name, code] {
            if (code.isEmpty()) {
                QMessageBox::warning(guardedParent, tr("Verification Code"), tr("Could not make a verification code for %1. They may not be in the encrypted group yet.").arg(name));
                return;
            }
            codeDialog(guardedParent, tr("Verification Code"), tr("Compare this code with <b>%1</b> to verify end-to-end encryption. Both of you should see the same code.").arg(name.toHtmlEscaped()), code)->show();
        });
    });
}

} // namespace UI
} // namespace Acheron
