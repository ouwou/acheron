#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>

#include <memory>

namespace Acheron {
namespace Core {
namespace Lottie {

[[nodiscard]] bool isSupported();

[[nodiscard]] bool looksLikeLottie(const QByteArray &data);

class Animation
{
public:
    ~Animation();

    [[nodiscard]] static std::unique_ptr<Animation> fromJson(const QByteArray &json);

    [[nodiscard]] int frameCount() const;
    [[nodiscard]] int frameDelayMs() const;
    [[nodiscard]] QImage render(int frame, const QSize &size) const;

private:
    struct Impl;
    explicit Animation(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl;
};

} // namespace Lottie
} // namespace Core
} // namespace Acheron
