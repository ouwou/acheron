#include "LottieDecoder.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#ifdef ACHERON_HAVE_RLOTTIE
#include <rlottie.h>
#endif

namespace Acheron {
namespace Core {
namespace Lottie {

bool looksLikeLottie(const QByteArray &data)
{
    const auto firstInk = std::find_if(data.cbegin(), data.cend(), [](char c) { return !std::isspace(static_cast<unsigned char>(c)); });
    return firstInk != data.cend() && *firstInk == '{';
}

#ifdef ACHERON_HAVE_RLOTTIE

namespace {
constexpr double MaxRenderedFps = 30.0;
} // namespace

struct Animation::Impl
{
    std::unique_ptr<rlottie::Animation> animation;
    int sourceFrames = 0;
    int sourceFramesPerRendered = 1;
    double sourceFps = 0;
};

bool isSupported()
{
    return true;
}

std::unique_ptr<Animation> Animation::fromJson(const QByteArray &json)
{
    const std::string modelCacheKey;
    const std::string externalResourceDir;
    auto animation = rlottie::Animation::loadFromData(json.toStdString(), modelCacheKey, externalResourceDir, false);
    if (!animation || animation->totalFrame() == 0 || animation->frameRate() <= 0)
        return nullptr;

    auto impl = std::make_unique<Impl>();
    impl->sourceFrames = int(animation->totalFrame());
    impl->sourceFps = animation->frameRate();
    impl->sourceFramesPerRendered = std::max(1, int(std::lround(impl->sourceFps / MaxRenderedFps)));
    impl->animation = std::move(animation);
    return std::unique_ptr<Animation>(new Animation(std::move(impl)));
}

int Animation::frameCount() const
{
    return (impl->sourceFrames + impl->sourceFramesPerRendered - 1) / impl->sourceFramesPerRendered;
}

int Animation::frameDelayMs() const
{
    return int(std::lround(impl->sourceFramesPerRendered * 1000.0 / impl->sourceFps));
}

QImage Animation::render(int frame, const QSize &size) const
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    rlottie::Surface surface(reinterpret_cast<uint32_t *>(image.bits()), size_t(image.width()),
                             size_t(image.height()), size_t(image.bytesPerLine()));
    impl->animation->renderSync(size_t(frame * impl->sourceFramesPerRendered), surface);
    return image;
}

#else

struct Animation::Impl
{
};

bool isSupported()
{
    return false;
}

std::unique_ptr<Animation> Animation::fromJson(const QByteArray &)
{
    return nullptr;
}

int Animation::frameCount() const
{
    return 0;
}

int Animation::frameDelayMs() const
{
    return 0;
}

QImage Animation::render(int, const QSize &) const
{
    return {};
}

#endif

Animation::Animation(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}

Animation::~Animation() = default;

} // namespace Lottie
} // namespace Core
} // namespace Acheron
