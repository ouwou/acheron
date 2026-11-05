#pragma once

#include <QWidget>

#include <memory>

#include "Core/Audio/VoiceSettings.hpp"

class QComboBox;
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
    void hideEvent(QHideEvent *event) override;

private:
    void fillDevices();
    void setTesting(bool testing);
    void onTestCaptured(const QByteArray &pcm);

    Core::Audio::VoiceSettings settings;
    std::unique_ptr<Core::Audio::IAudioBackend> testBackend;
    QComboBox *inputDevice;
    QComboBox *outputDevice;
    QPushButton *testButton;
    VolumeMeter *testMeter;
};

} // namespace UI
} // namespace Acheron
