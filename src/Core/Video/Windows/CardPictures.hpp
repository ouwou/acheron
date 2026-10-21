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

[[nodiscard]] AVBufferRef *cardPictures(AVBufferRef *device, const QSize &size);

[[nodiscard]] bool prepareBgra(AVFrame *bgra, const QSize &size);
[[nodiscard]] int readBack(const AVFrame *onCard, AVFrame *memory);
// no picture of a window that has drawn itself is transparent everywhere sampled
[[nodiscard]] bool isEmptyPicture(const AVFrame *onCard, AVFrame *scratch);

// ddagrab hands the last picture over again while the screen shows nothing new. holding a reference to
// it in `last` keeps its slot on the card from being reused
[[nodiscard]] bool isSamePictureOnCard(const AVFrame *last, const AVFrame *now);
void keepReferenceTo(AVFrame *last, const AVFrame *now);

[[nodiscard]] QRect wholeOf(const AVFrame *picture);
[[nodiscard]] QRect scaledPart(const QRect &part, const QSize &from, const QSize &to);

[[nodiscard]] bool cutOntoCard(const AVFrame *from, const QRect &area, AVBufferRef *pool, const QPoint &at, AVFrame *to);

} // namespace Win
} // namespace Video
} // namespace Core
} // namespace Acheron

#endif
