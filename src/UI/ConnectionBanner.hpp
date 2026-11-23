#pragma once

#include <QWidget>
#include <QLabel>
#include <QTimer>
#include <QPalette>

namespace Acheron {
namespace UI {

class ConnectionBanner : public QWidget
{
    Q_OBJECT
public:
    explicit ConnectionBanner(QWidget *parent = nullptr);

    void showReconnecting(int attempt);
    void hide();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void updateDots();
    void applyPaletteColors();

    QLabel *label;
    QTimer *dotTimer;
    QString baseText;
    int dotCount = 0;
};

} // namespace UI
} // namespace Acheron
