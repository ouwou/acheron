#include "AudioPage.hpp"

#include "Core/Audio/AudioBackends.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

AudioPage::AudioPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("Backend *"), this));
    row->addStretch(1);

    backendCombo = new QComboBox(this);
    backendCombo->addItem(tr("Automatic"), QString());
    for (const QString &name : Core::Audio::supportedAudioBackends())
        backendCombo->addItem(name, name);

    const int current = backendCombo->findData(Core::Audio::configuredAudioBackend());
    backendCombo->setCurrentIndex(current >= 0 ? current : 0);

    row->addWidget(backendCombo);
    layout->addLayout(row);

    auto *note = new QLabel(tr("* Takes effect after a restart."), this);
    QFont noteFont = note->font();
    noteFont.setItalic(true);
    if (noteFont.pointSize() > 1)
        noteFont.setPointSize(noteFont.pointSize() - 1);
    note->setFont(noteFont);
    layout->addWidget(note);

    layout->addStretch(1);

    connect(backendCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        Core::Audio::setConfiguredAudioBackend(backendCombo->itemData(index).toString());
    });
}

} // namespace UI
} // namespace Acheron
