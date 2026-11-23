#pragma once

#include <QWidget>

#include <optional>

namespace Acheron {
namespace UI {

class VolumeMeter : public QWidget
{
    Q_OBJECT
public:
    explicit VolumeMeter(QWidget *parent = nullptr);

    void setLevel(float rms);
    void setThreshold(std::optional<float> threshold);

    QSize sizeHint() const override { return QSize(100, 8); }
    QSize minimumSizeHint() const override { return QSize(24, 4); }

    static constexpr float MAX_RMS = 2000.0f;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    float level = 0.0f;
    std::optional<float> threshold;
};

} // namespace UI
} // namespace Acheron
