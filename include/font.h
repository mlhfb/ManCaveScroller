#ifndef FONT_H
#define FONT_H

#include <stdint.h>

#define FONT_WIDTH      5
#define FONT_HEIGHT     7
#define FONT_FIRST_CHAR 32  // space
#define FONT_LAST_CHAR  126 // tilde

// Returns pointer to 5 bytes of column data for the given character.
// Each byte represents one column (bit 0 = top row, bit 6 = bottom row).
// Returns NULL for unsupported characters.
const uint8_t *font_get_glyph(char c);

#endif
