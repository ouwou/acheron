#pragma once

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif

// decoding left on for the call sounds (wav, mp3, flac)
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif
