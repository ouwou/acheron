#pragma once

#include <QCoreApplication>
#include <QString>

#include <functional>

#include "Core/Snowflake.hpp"

class QMenu;
class QWidget;

namespace Acheron {
namespace Core {
namespace Audio {
class VoiceManager;
}
} // namespace Core

namespace UI {

class VoiceActions
{
    Q_DECLARE_TR_FUNCTIONS(VoiceActions)

public:
    static void addVolumeSlider(QMenu *menu, const QString &label, float gain, const std::function<void(float)> &changed);
    static void addUserActions(QMenu *menu, Core::Audio::VoiceManager *voice, Core::Snowflake userId, const QString &name);
    static void fillDeviceMenu(QMenu *menu, Core::Audio::VoiceManager *voice, const std::function<void()> &openVoiceSettings);
    static void showPrivacyCode(QWidget *parent, Core::Audio::VoiceManager *voice);

private:
    static void showVerificationCode(QWidget *parent, Core::Audio::VoiceManager *voice, Core::Snowflake userId, const QString &name);
};

} // namespace UI
} // namespace Acheron
