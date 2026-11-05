#ifndef UI_FONT_H
#define UI_FONT_H

#include <stdint.h>

// Bitmap font: one byte per row (MSB = leftmost pixel), glyphs for
// 0x20-0x7E followed by 0xA0-0xFF (Latin-1).
typedef struct {
    uint8_t w;
    uint8_t h;
    const uint8_t *data;
} font_t;

extern const font_t font_6x12;
extern const font_t font_8x16;

#endif
