#pragma once

#include <QWidget>

namespace Acheron {
namespace UI {

// Fork: Settings > About, showing which build this is (see fork/README.md).
class AboutPage : public QWidget
{
public:
    explicit AboutPage(QWidget *parent = nullptr);
};

} // namespace UI
} // namespace Acheron
