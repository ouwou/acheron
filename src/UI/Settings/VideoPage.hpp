#pragma once

#include <QList>
#include <QWidget>

class QCheckBox;

namespace Acheron {
namespace UI {

class VideoPage : public QWidget
{
    Q_OBJECT
public:
    explicit VideoPage(QWidget *parent = nullptr);

private:
    QList<QCheckBox *> sendCodecChecks;
    QList<QCheckBox *> receiveCodecChecks;
};

} // namespace UI
} // namespace Acheron
