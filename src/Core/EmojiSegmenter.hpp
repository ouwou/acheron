#pragma once

#include <QString>

namespace Acheron {
namespace Core {

// https://github.com/google/emoji-segmenter/blob/1cada87c62550446fca6a42a69743688b4539a4c/emoji_presentation_scanner.rl#L30-L45
enum CharacterCategory {
    EMOJI = 0,
    EMOJI_TEXT_PRESENTATION = 1,
    EMOJI_EMOJI_PRESENTATION = 2,
    EMOJI_MODIFIER_BASE = 3,
    EMOJI_MODIFIER = 4,
    EMOJI_VS_BASE = 5,
    REGIONAL_INDICATOR = 6,
    KEYCAP_BASE = 7,
    COMBINING_ENCLOSING_KEYCAP = 8,
    COMBINING_ENCLOSING_CIRCLE_BACKSLASH = 9,
    ZWJ = 10,
    VS15 = 11,
    VS16 = 12,
    TAG_BASE = 13,
    TAG_SEQUENCE = 14,
    TAG_TERM = 15,
    OTHER = 16
};

typedef CharacterCategory *emoji_text_iter_t;

// Count the number of emoji presentation sequences in a text string.
// Returns -1 if any non-emoji, non-whitespace content is found.
// Returns the emoji sequence count (0 if empty/all whitespace).
int countUnicodeEmojisSegmented(const QString &text);

} // namespace Core
} // namespace Acheron
