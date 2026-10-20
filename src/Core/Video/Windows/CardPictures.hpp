#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

#include <QPoint>
#include <QRect>
#include <QSize>

struct AVBufferRef;
struct AVFrame;

namespace Acheron {
namespace Core {
namespace Video {
namespace Win {

// Pictures the capture keeps on the graphics card, Direct3D 11 textures that ffmpeg holds as
// frames, and the few things it does with them there.

// Frames on `device` for BGRA pictures of one size. Null when the card will not make them.
[[nodiscard]] AVBufferRef *cardPictures(AVBufferRef *device, const QSize &size);

// `bgra` made a writable BGRA picture of `size` in memory, kept as it is when it is one.
[[nodiscard]] bool prepareBgra(AVFrame *bgra, const QSize &size);
// Into memory in the format the card has it in, into what `memory` holds already when that
// fits. Negative, an ffmpeg error, when it could not be read.
[[nodiscard]] int readBack(const AVFrame *onCard, AVFrame *memory);
// Whether a BGRA picture on the card is transparent wherever it is looked at, which no picture
// of a window that has drawn itself is. `scratch` is what it is read back into.
[[nodiscard]] bool isEmptyPicture(const AVFrame *onCard, AVFrame *scratch);

// ddagrab hands the picture it handed over last over again while the screen shows nothing
// new. `last` is a reference to that one, which keeps its place on the card from being given
// to another.
[[nodiscard]] bool isSamePictureOnCard(const AVFrame *last, const AVFrame *now);
void keepReferenceTo(AVFrame *last, const AVFrame *now);

[[nodiscard]] QRect wholeOf(const AVFrame *picture);
// Where `part` of a picture lies once the picture is scaled from one size to another.
[[nodiscard]] QRect scaledPart(const QRect &part, const QSize &from, const QSize &to);

// `area` of one picture on the graphics card as a picture of its own from `pool`: at `at` of
// it, with black wherever the area does not reach.
[[nodiscard]] bool cutOntoCard(const AVFrame *from, const QRect &area, AVBufferRef *pool, const QPoint &at, AVFrame *to);

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
