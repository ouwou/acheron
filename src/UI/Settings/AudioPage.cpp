#include "AudioPage.hpp"

#include "Core/Audio/AudioBackends.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

QComboBox *makeAudioBackendCombo(QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    combo->addItem(AudioPage::tr("Automatic"), QString());
    for (const QString &name : Core::Audio::supportedAudioBackends())
        combo->addItem(name, name);

    const int current = combo->findData(Core::Audio::configuredAudioBackend());
    combo->setCurrentIndex(current >= 0 ? current : 0);
    QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), combo, [combo](int index) {
        Core::Audio::setConfiguredAudioBackend(combo->itemData(index).toString());
    });
    return combo;
}

AudioPage::AudioPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("Backend *"), this));
    row->addStretch(1);
    row->addWidget(makeAudioBackendCombo(this));
    layout->addLayout(row);

    auto *note = new QLabel(tr("* Takes effect after a restart."), this);
    QFont noteFont = note->font();
    noteFont.setItalic(true);
    if (noteFont.pointSize() > 1)
        noteFont.setPointSize(noteFont.pointSize() - 1);
    note->setFont(noteFont);
    layout->addWidget(note);

    layout->addStretch(1);
}

} // namespace UI
} // namespace Acheron
