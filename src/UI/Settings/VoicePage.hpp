#pragma once

#include <QWidget>

#include <functional>
#include <memory>
#include <vector>

#include "Core/Audio/Sounds.hpp"
#include "Core/Audio/VoiceSettings.hpp"

class QComboBox;
class QFormLayout;
class QPushButton;

namespace Acheron {
namespace Core {
namespace Audio {
class IAudioBackend;
}
} // namespace Core

namespace UI {

class VolumeMeter;

class VoicePage : public QWidget
{
    Q_OBJECT
public:
    explicit VoicePage(QWidget *parent = nullptr);
    ~VoicePage() override;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void fillDevices();
    void setTesting(bool testing);
    void onTestCaptured(const QByteArray &pcm);
    void addSoundRow(QFormLayout *form, QWidget *parent, Core::Audio::Sound sound);
    void playSound(Core::Audio::Sound sound);

    Core::Audio::VoiceSettings settings;
    std::unique_ptr<Core::Audio::IAudioBackend> testBackend;
    QComboBox *inputDevice;
    QComboBox *outputDevice;
    QPushButton *testButton;
    VolumeMeter *testMeter;
    // the folder can change behind the page's back
    std::vector<std::function<void()>> soundRowRefreshers;
};

} // namespace UI
} // namespace Acheron
