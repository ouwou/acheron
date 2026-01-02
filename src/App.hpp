#pragma once

#include <QtWidgets/QApplication>

namespace Acheron {
class App : public QApplication
{
    Q_OBJECT
public:
    explicit App(int &argc, char **argv);
};
} // namespace Acheron
