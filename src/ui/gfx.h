#ifndef UI_GFX_H
#define UI_GFX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui/font.h"
#include "hw/lcd.h"

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define RGB888_TO_565(c) RGB565(((c) >> 16) & 0xFF, ((c) >> 8) & 0xFF, (c) & 0xFF)

// Dark theme modelled on the Meshtastic Android app
#define C_BG RGB565(0x12, 0x14, 0x16)
#define C_SURFACE RGB565(0x24, 0x28, 0x2c)
#define C_SURFACE2 RGB565(0x34, 0x3a, 0x40)
#define C_BAR RGB565(0x1c, 0x20, 0x24)
#define C_TEXT RGB565(0xe8, 0xea, 0xec)
#define C_DIM RGB565(0x96, 0x9c, 0xa2)
#define C_FAINT RGB565(0x5a, 0x60, 0x66)
#define C_ACCENT RGB565(0x67, 0xea, 0x94)
#define C_ACCENT_DARK RGB565(0x2c, 0x6e, 0x48)
#define C_ON_ACCENT RGB565(0x00, 0x00, 0x00)
#define C_RED RGB565(0xf2, 0x5a, 0x5a)
#define C_YELLOW RGB565(0xf2, 0xc9, 0x4c)
#define C_BLUE RGB565(0x5a, 0xa8, 0xf2)
#define C_WHITE 0xFFFF
#define C_BLACK 0x0000
#define C_BUBBLE_ME RGB565(0x2c, 0x6e, 0x48)
#define C_BUBBLE_THEM RGB565(0x34, 0x3a, 0x40)

#define TRANSPARENT (-1)

void gfx_clip(int x, int y, int w, int h);
void gfx_clip_reset(void);

void gfx_clear(uint16_t c);
void gfx_pixel(int x, int y, uint16_t c);
void gfx_fill(int x, int y, int w, int h, uint16_t c);
void gfx_hline(int x, int y, int w, uint16_t c);
void gfx_vline(int x, int y, int h, uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);
void gfx_round_fill(int x, int y, int w, int h, int r, uint16_t c);
void gfx_line(int x0, int y0, int x1, int y1, uint16_t c);
void gfx_circle(int cx, int cy, int r, uint16_t c);
void gfx_fill_circle(int cx, int cy, int r, uint16_t c);

// UTF-8 helpers: decode next code point, advancing *s.
uint32_t utf8_next(const char **s);
int utf8_len(const char *s); // number of code points

// Draws text (UTF-8, rendered as Latin-1). bg = TRANSPARENT or RGB565.
// Returns the x coordinate after the last character. Stops at '\n'.
int gfx_text(int x, int y, const char *s, const font_t *f, uint16_t fg, int32_t bg);
int gfx_text_n(int x, int y, const char *s, int max_chars, const font_t *f, uint16_t fg, int32_t bg);
// Draws at most nbytes bytes of s.
int gfx_text_b(int x, int y, const char *s, int nbytes, const font_t *f, uint16_t fg, int32_t bg);
// Truncates with "..." to fit max_w pixels.
int gfx_text_fit(int x, int y, const char *s, int max_w, const font_t *f, uint16_t fg, int32_t bg);
int gfx_text_width(const char *s, const font_t *f);
void gfx_text_center(int cx, int y, const char *s, const font_t *f, uint16_t fg, int32_t bg);
void gfx_text_right(int rx, int y, const char *s, const font_t *f, uint16_t fg, int32_t bg);
int gfx_printf(int x, int y, const font_t *f, uint16_t fg, int32_t bg, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

// Picks black or white text for the given background.
uint16_t gfx_contrast(uint16_t bg);

#endif
