#pragma once

#include <QRect>
#include <QSize>

#include <memory>

struct AVFrame;

namespace Acheron {
namespace Core {
namespace Video {

// Direct3D 11's video processor. not ffmpeg's scale_d3d11: it allocates a 10-texture array a Radeon RX
// 7800 XT wont make of NV12, cant crop, and leaves the colours to the driver. not thread safe
class CardScaler
{
public:
    struct Output
    {
        QSize size;
        bool bt601 = false;
        bool rgbWillDo = false;

        bool operator==(const Output &) const = default;
    };

    CardScaler();
    ~CardScaler();

    CardScaler(const CardScaler &) = delete;
    CardScaler &operator=(const CardScaler &) = delete;

    [[nodiscard]] static bool usable();

    bool scale(const AVFrame *from, const QRect &area, const Output &output, const QRect &placed, AVFrame *into);
    bool scale(const AVFrame *from, const Output &output, AVFrame *into);

private:
    struct Card;
    std::unique_ptr<Card> card;
};

} // namespace Video
} // namespace Core
} // namespace Acheron
