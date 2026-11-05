#include "VoicePage.hpp"

#include "AudioPage.hpp"
#include "Core/Audio/IAudioBackend.hpp"
#include "UI/AdvancedToggle.hpp"
#include "UI/VolumeMeter.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <functional>

namespace Acheron {
namespace UI {

namespace {

using Core::Audio::AudioDeviceInfo;
using Core::Audio::VoiceSettings;

constexpr int MIN_OPUS_BITRATE_KBPS = 8;
constexpr int MAX_OPUS_BITRATE_KBPS = 510;
constexpr int MAX_OPUS_COMPLEXITY = 10;
constexpr int MAX_PACKET_LOSS_PERCENT = 100;

QFormLayout *newForm()
{
    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setContentsMargins(0, 0, 0, 0);
    return form;
}

QSlider *addSlider(QFormLayout *form, QWidget *parent, const QString &label, int minimum, int maximum, int value, const std::function<QString(int)> &shown)
{
    auto *field = new QWidget(parent);
    auto *row = new QHBoxLayout(field);
    row->setContentsMargins(0, 0, 0, 0);
    auto *slider = new QSlider(Qt::Horizontal, field);
    slider->setRange(minimum, maximum);
    slider->setValue(value);
    row->addWidget(slider, 1);
    auto *valueLabel = new QLabel(shown(value), field);
    valueLabel->setMinimumWidth(valueLabel->fontMetrics().horizontalAdvance(shown(maximum)));
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(valueLabel);
    form->addRow(label, field);
    QObject::connect(slider, &QSlider::valueChanged, valueLabel, [valueLabel, shown](int chosen) { valueLabel->setText(shown(chosen)); });
    return slider;
}

QString percentText(int value)
{
    return QStringLiteral("%1%").arg(value);
}

QString plainNumber(int value)
{
    return QString::number(value);
}

void fillDeviceCombo(QComboBox *combo, const QList<AudioDeviceInfo> &devices, const QByteArray &chosen)
{
    combo->clear();
    combo->addItem(VoicePage::tr("Default"), QByteArray());
    for (const AudioDeviceInfo &device : devices)
        combo->addItem(device.description, device.id);
    combo->setCurrentIndex(qMax(0, combo->findData(chosen)));
}

QLabel *restartNote(QWidget *parent)
{
    auto *note = new QLabel(VoicePage::tr("* Takes effect after a restart."), parent);
    QFont noteFont = note->font();
    noteFont.setItalic(true);
    if (noteFont.pointSize() > 1)
        noteFont.setPointSize(noteFont.pointSize() - 1);
    note->setFont(noteFont);
    return note;
}

} // namespace

VoicePage::VoicePage(QWidget *parent)
    : QWidget(parent), settings(VoiceSettings::load()), testBackend(Core::Audio::IAudioBackend::create())
{
    auto *layout = new QVBoxLayout(this);
    QFormLayout *form = newForm();
    layout->addLayout(form);

    form->addRow(tr("Backend *"), makeAudioBackendCombo(this));

    inputDevice = new QComboBox(this);
    form->addRow(tr("Input device"), inputDevice);
    outputDevice = new QComboBox(this);
    form->addRow(tr("Output device"), outputDevice);
    fillDevices();
    connect(testBackend.get(), &Core::Audio::IAudioBackend::devicesChanged, this, &VoicePage::fillDevices);
    connect(inputDevice, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        settings.inputDevice = inputDevice->itemData(index).toByteArray();
        testBackend->setInputDevice(settings.inputDevice);
        settings.saveAndApply();
    });
    connect(outputDevice, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        settings.outputDevice = outputDevice->itemData(index).toByteArray();
        settings.saveAndApply();
    });

    QSlider *inputGain = addSlider(form, this, tr("Input volume"), 0, VoiceSettings::MAX_GAIN_PERCENT, settings.inputGainPercent, percentText);
    connect(inputGain, &QSlider::valueChanged, this, [this](int percent) {
        settings.inputGainPercent = percent;
        testBackend->setInputGain(static_cast<float>(percent) / VoiceSettings::UNCHANGED_PERCENT);
        settings.saveAndApply();
    });
    QSlider *outputVolume = addSlider(form, this, tr("Output volume"), 0, VoiceSettings::MAX_GAIN_PERCENT, settings.outputVolumePercent, percentText);
    connect(outputVolume, &QSlider::valueChanged, this, [this](int percent) {
        settings.outputVolumePercent = percent;
        settings.saveAndApply();
    });
    QSlider *sensitivity = addSlider(form, this, tr("Input sensitivity"), 0, VoiceSettings::MAX_VAD_THRESHOLD, settings.vadThreshold, plainNumber);
    connect(sensitivity, &QSlider::valueChanged, this, [this](int threshold) {
        settings.vadThreshold = threshold;
        testMeter->setThreshold(static_cast<float>(threshold));
        settings.saveAndApply();
    });

    testButton = new QPushButton(tr("Test"), this);
    testButton->setCheckable(true);
    testButton->setToolTip(tr("Shows how loud the microphone is against the sensitivity, without sending anything"));
    auto *meterField = new QWidget(this);
    auto *meterRow = new QHBoxLayout(meterField);
    meterRow->setContentsMargins(0, 0, 0, 0);
    testMeter = new VolumeMeter(meterField);
    testMeter->setThreshold(static_cast<float>(settings.vadThreshold));
    meterRow->addWidget(testMeter);
    form->addRow(testButton, meterField);
    connect(testButton, &QPushButton::toggled, this, &VoicePage::setTesting);
    connect(testBackend.get(), &Core::Audio::IAudioBackend::audioCaptured, this, &VoicePage::onTestCaptured);

#ifdef ACHERON_HAVE_RNNOISE
    auto *noiseSuppression = new QCheckBox(tr("Noise suppression"), this);
    noiseSuppression->setChecked(settings.noiseSuppression);
    form->addRow(noiseSuppression);
    connect(noiseSuppression, &QCheckBox::toggled, this, [this](bool enabled) {
        settings.noiseSuppression = enabled;
        settings.saveAndApply();
    });
#endif

    auto *advanced = new QWidget(this);
    QFormLayout *advancedForm = newForm();
    advanced->setLayout(advancedForm);

#ifdef ACHERON_HAVE_RNNOISE
    auto *rnnoiseVad = new QCheckBox(tr("Detect speech with RNNoise"), advanced);
    rnnoiseVad->setChecked(settings.rnnoiseVad);
    advancedForm->addRow(rnnoiseVad);
    connect(rnnoiseVad, &QCheckBox::toggled, this, [this](bool enabled) {
        settings.rnnoiseVad = enabled;
        settings.saveAndApply();
    });
#endif

    auto *application = new QComboBox(advanced);
    application->addItem(tr("VoIP"), OPUS_APPLICATION_VOIP);
    application->addItem(tr("Audio"), OPUS_APPLICATION_AUDIO);
    application->setCurrentIndex(qMax(0, application->findData(settings.opusApplication)));
    advancedForm->addRow(tr("Opus application"), application);
    connect(application, qOverload<int>(&QComboBox::activated), this, [this, application](int index) {
        settings.opusApplication = application->itemData(index).toInt();
        settings.saveAndApply();
    });

    QSlider *bitrate = addSlider(advancedForm, advanced, tr("Bitrate"), MIN_OPUS_BITRATE_KBPS, MAX_OPUS_BITRATE_KBPS, settings.opusBitrateKbps, [](int kbps) { return tr("%1 kbps").arg(kbps); });
    connect(bitrate, &QSlider::valueChanged, this, [this](int kbps) {
        settings.opusBitrateKbps = kbps;
        settings.saveAndApply();
    });
    QSlider *complexity = addSlider(advancedForm, advanced, tr("Complexity"), 0, MAX_OPUS_COMPLEXITY, settings.opusComplexity, plainNumber);
    connect(complexity, &QSlider::valueChanged, this, [this](int value) {
        settings.opusComplexity = value;
        settings.saveAndApply();
    });

    auto *signal = new QComboBox(advanced);
    signal->addItem(tr("Automatic"), OPUS_AUTO);
    signal->addItem(tr("Voice"), OPUS_SIGNAL_VOICE);
    signal->addItem(tr("Music"), OPUS_SIGNAL_MUSIC);
    signal->setCurrentIndex(qMax(0, signal->findData(settings.opusSignalType)));
    advancedForm->addRow(tr("Signal"), signal);
    connect(signal, qOverload<int>(&QComboBox::activated), this, [this, signal](int index) {
        settings.opusSignalType = signal->itemData(index).toInt();
        settings.saveAndApply();
    });

    auto *fec = new QCheckBox(tr("Forward error correction"), advanced);
    fec->setChecked(settings.opusFec);
    advancedForm->addRow(fec);
    connect(fec, &QCheckBox::toggled, this, [this](bool enabled) {
        settings.opusFec = enabled;
        settings.saveAndApply();
    });
    QSlider *packetLoss = addSlider(advancedForm, advanced, tr("Expected packet loss"), 0, MAX_PACKET_LOSS_PERCENT, settings.opusPacketLossPercent, percentText);
    connect(packetLoss, &QSlider::valueChanged, this, [this](int percent) {
        settings.opusPacketLossPercent = percent;
        settings.saveAndApply();
    });

    layout->addWidget(makeAdvancedToggle(advanced, this));
    layout->addWidget(advanced);
    layout->addWidget(restartNote(this));
    layout->addStretch(1);
}

VoicePage::~VoicePage() = default;

void VoicePage::hideEvent(QHideEvent *event)
{
    testButton->setChecked(false);
    QWidget::hideEvent(event);
}

void VoicePage::fillDevices()
{
    fillDeviceCombo(inputDevice, testBackend->availableInputDevices(), settings.inputDevice);
    fillDeviceCombo(outputDevice, testBackend->availableOutputDevices(), settings.outputDevice);
}

void VoicePage::setTesting(bool testing)
{
    testButton->setText(testing ? tr("Stop") : tr("Test"));
    if (!testing) {
        testBackend->stopCapture();
        testMeter->setLevel(0.0f);
        return;
    }
    testBackend->setInputDevice(settings.inputDevice);
    testBackend->setInputGain(static_cast<float>(settings.inputGainPercent) / VoiceSettings::UNCHANGED_PERCENT);
    testBackend->startCapture();
}

void VoicePage::onTestCaptured(const QByteArray &pcm)
{
    testMeter->setLevel(Core::Audio::rmsOf(reinterpret_cast<const int16_t *>(pcm.constData()), static_cast<int>(pcm.size() / static_cast<int>(sizeof(int16_t)))));
}

} // namespace UI
} // namespace Acheron
