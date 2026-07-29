#pragma once

#include <QtGlobal>

// stdext::make_checked_array_iterator is deprecated in later msvc  
#if defined(_MSC_VER) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#undef QT_MAKE_CHECKED_ARRAY_ITERATOR
#undef QT_MAKE_UNCHECKED_ARRAY_ITERATOR
#define QT_MAKE_CHECKED_ARRAY_ITERATOR(x, N) (x)
#define QT_MAKE_UNCHECKED_ARRAY_ITERATOR(x) (x)
#endif

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QtCore/qhashfunctions.h>
#include <QtCore/qsize.h>

inline size_t qHash(const QSize &key, size_t seed = 0) noexcept
{
    return qHash(key.width(), qHash(key.height(), seed));
}

template <typename... Args>
inline size_t qHashMulti(size_t seed, const Args &...args)
{
    ((seed = qHash(args, seed)), ...);
    return seed;
}
#endif

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#define AnchorAtOffsetMatchOption AnchoredMatchOption
#endif
