#pragma once

#include <QList>
#include <QWidget>

#include "Core/Emoji/PickerEmoji.hpp"
#include "Core/Snowflake.hpp"

namespace Acheron {

namespace Core {
class ImageManager;
}

namespace UI {

class PaintedImages;

class EmojiSectionRail : public QWidget
{
    Q_OBJECT
public:
    static constexpr int Width = 36;

    explicit EmojiSectionRail(QWidget *parent = nullptr);

    void setImageSource(Core::ImageManager *imageManager, Core::Snowflake accountId);
    void setSections(const QList<Core::PickerSection> &sections);
    void setCurrent(int section);

signals:
    void sectionClicked(int section);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *event) override;

private:
    [[nodiscard]] int sectionAt(const QPoint &pos) const;
    [[nodiscard]] QRect itemRect(int section) const;
    void setOffset(int newOffset);
    void setHovered(int section);

    PaintedImages *images;
    QList<Core::PickerSection> shownSections;
    int current = -1;
    int hovered = -1;
    int offset = 0;
};

} // namespace UI
} // namespace Acheron
