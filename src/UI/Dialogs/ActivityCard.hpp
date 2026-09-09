#pragma once

#include <QWidget>

#include "Core/Presence/ActivityFormat.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

class QLabel;

namespace Acheron {
namespace Core {
class ImageManager;
}

namespace UI {

class ActivityCard : public QWidget
{
    Q_OBJECT
public:
    ActivityCard(const Discord::Activity &activity, const QUrl &fallbackImage,
                 Core::ImageManager *images, Core::Snowflake accountId,
                 QWidget *parent = nullptr);

    void tick(qint64 nowMs);

    [[nodiscard]] bool hasTimer() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyActivity(const Discord::Activity &activity, const QUrl &fallbackImage);
    void updateProgressFill();

    Core::ImageManager *images;
    Core::Snowflake accountId;

    QLabel *headingLabel;
    QLabel *largeImageLabel;
    QLabel *smallImageLabel;
    QLabel *nameLabel;
    QLabel *detailsLabel;
    QLabel *stateLabel;
    QLabel *timerLabel;
    QWidget *progressBar;
    QWidget *progressFill;

    Core::ActivityFormat::TimerInfo timer;
    double fillRatio = 0.0;
};

} // namespace UI
} // namespace Acheron
