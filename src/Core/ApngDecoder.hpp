#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>

#include <memory>

namespace Acheron {
namespace Core {
namespace Apng {

class Reader
{
public:
    ~Reader();

    // null unless this ffmpeg is enabled and this is apng (not png)
    [[nodiscard]] static std::unique_ptr<Reader> open(const QByteArray &png);

    [[nodiscard]] QSize canvasSize() const;
    [[nodiscard]] int frameCount() const;

    bool next(QImage &canvas, int &delayMs);

private:
    struct Impl;
    explicit Reader(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl;
};

} // namespace Apng
} // namespace Core
} // namespace Acheron
