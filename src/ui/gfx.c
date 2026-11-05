#include "ui/gfx.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int clip_x0 = 0, clip_y0 = 0, clip_x1 = LCD_W, clip_y1 = LCD_H;

void gfx_clip(int x, int y, int w, int h) {
    clip_x0 = x < 0 ? 0 : x;
    clip_y0 = y < 0 ? 0 : y;
    clip_x1 = x + w > LCD_W ? LCD_W : x + w;
    clip_y1 = y + h > LCD_H ? LCD_H : y + h;
}

void gfx_clip_reset(void) {
    clip_x0 = clip_y0 = 0;
    clip_x1 = LCD_W;
    clip_y1 = LCD_H;
}

void gfx_clear(uint16_t c) {
    for (int i = 0; i < LCD_W * LCD_H; i++) lcd_fb[i] = c;
}

void gfx_pixel(int x, int y, uint16_t c) {
    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1) return;
    lcd_fb[y * LCD_W + x] = c;
}

void gfx_fill(int x, int y, int w, int h, uint16_t c) {
    int x0 = x < clip_x0 ? clip_x0 : x;
    int y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w;
    int y1 = y + h > clip_y1 ? clip_y1 : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *p = &lcd_fb[yy * LCD_W + x0];
        for (int xx = x0; xx < x1; xx++) *p++ = c;
    }
}

void gfx_hline(int x, int y, int w, uint16_t c) { gfx_fill(x, y, w, 1, c); }
void gfx_vline(int x, int y, int h, uint16_t c) { gfx_fill(x, y, 1, h, c); }

void gfx_rect(int x, int y, int w, int h, uint16_t c) {
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y, h, c);
    gfx_vline(x + w - 1, y, h, c);
}

void gfx_round_fill(int x, int y, int w, int h, int r, uint16_t c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    gfx_fill(x, y + r, w, h - 2 * r, c);
    for (int i = 0; i < r; i++) {
        // inset for row i from the top/bottom edge
        int dy = r - i;
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) dx++;
        int inset = r - dx;
        gfx_hline(x + inset, y + i, w - 2 * inset, c);
        gfx_hline(x + inset, y + h - 1 - i, w - 2 * inset, c);
    }
}

void gfx_line(int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_circle(int cx, int cy, int r, uint16_t c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(cx + x, cy + y, c);
        gfx_pixel(cx + y, cy + x, c);
        gfx_pixel(cx - y, cy + x, c);
        gfx_pixel(cx - x, cy + y, c);
        gfx_pixel(cx - x, cy - y, c);
        gfx_pixel(cx - y, cy - x, c);
        gfx_pixel(cx + y, cy - x, c);
        gfx_pixel(cx + x, cy - y, c);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void gfx_fill_circle(int cx, int cy, int r, uint16_t c) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) dx++;
        gfx_hline(cx - dx, cy + dy, 2 * dx + 1, c);
    }
}

uint32_t utf8_next(const char **s) {
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p;
    if (!c) return 0;
    int extra = 0;
    if (c < 0x80) {
        extra = 0;
    } else if ((c & 0xE0) == 0xC0) {
        c &= 0x1F;
        extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
        c &= 0x0F;
        extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
        c &= 0x07;
        extra = 3;
    } else {
        *s += 1;
        return 0xFFFD;
    }
    p++;
    for (int i = 0; i < extra; i++) {
        if ((*p & 0xC0) != 0x80) {
            *s = (const char *)p;
            return 0xFFFD;
        }
        c = (c << 6) | (*p++ & 0x3F);
    }
    *s = (const char *)p;
    return c;
}

int utf8_len(const char *s) {
    int n = 0;
    while (*s) {
        utf8_next(&s);
        n++;
    }
    return n;
}

static const uint8_t *glyph(const font_t *f, uint32_t cp) {
    int idx;
    if (cp >= 0x20 && cp <= 0x7E) {
        idx = (int)cp - 0x20;
    } else if (cp >= 0xA0 && cp <= 0xFF) {
        idx = 95 + (int)cp - 0xA0;
    } else {
        switch (cp) {
        case 0x2018: case 0x2019: idx = '\'' - 0x20; break;
        case 0x201C: case 0x201D: idx = '"' - 0x20; break;
        case 0x2013: case 0x2014: idx = '-' - 0x20; break;
        case 0x2026: idx = '.' - 0x20; break;
        default: idx = '?' - 0x20; break;
        }
    }
    return f->data + idx * f->h;
}

static void draw_glyph(int x, int y, const uint8_t *g, const font_t *f, uint16_t fg, int32_t bg) {
    if (x >= clip_x1 || y >= clip_y1 || x + f->w <= clip_x0 || y + f->h <= clip_y0) return;
    for (int r = 0; r < f->h; r++) {
        int yy = y + r;
        if (yy < clip_y0 || yy >= clip_y1) continue;
        uint8_t bits = g[r];
        uint16_t *row = &lcd_fb[yy * LCD_W];
        for (int c = 0; c < f->w; c++) {
            int xx = x + c;
            if (xx < clip_x0 || xx >= clip_x1) continue;
            if (bits & (0x80 >> c)) row[xx] = fg;
            else if (bg >= 0) row[xx] = (uint16_t)bg;
        }
    }
}

int gfx_text_n(int x, int y, const char *s, int max_chars, const font_t *f, uint16_t fg, int32_t bg) {
    int n = 0;
    while (*s && *s != '\n' && n < max_chars) {
        uint32_t cp = utf8_next(&s);
        if (cp >= 0xFE00 && cp <= 0xFE0F) continue; // variation selectors
        if (cp == '\r') continue;
        draw_glyph(x, y, glyph(f, cp), f, fg, bg);
        x += f->w;
        n++;
    }
    return x;
}

int gfx_text(int x, int y, const char *s, const font_t *f, uint16_t fg, int32_t bg) {
    return gfx_text_n(x, y, s, 0x7FFFFFFF, f, fg, bg);
}

int gfx_text_b(int x, int y, const char *s, int nbytes, const font_t *f, uint16_t fg, int32_t bg) {
    const char *end = s + nbytes;
    while (s < end && *s && *s != '\n') {
        uint32_t cp = utf8_next(&s);
        if ((cp >= 0xFE00 && cp <= 0xFE0F) || cp == '\r') continue;
        draw_glyph(x, y, glyph(f, cp), f, fg, bg);
        x += f->w;
    }
    return x;
}

int gfx_text_width(const char *s, const font_t *f) {
    int n = 0;
    while (*s && *s != '\n') {
        uint32_t cp = utf8_next(&s);
        if ((cp >= 0xFE00 && cp <= 0xFE0F) || cp == '\r') continue;
        n++;
    }
    return n * f->w;
}

int gfx_text_fit(int x, int y, const char *s, int max_w, const font_t *f, uint16_t fg, int32_t bg) {
    int max_chars = max_w / f->w;
    if (max_chars <= 0) return x;
    if (gfx_text_width(s, f) <= max_w) return gfx_text(x, y, s, f, fg, bg);
    if (max_chars <= 3) return gfx_text_n(x, y, s, max_chars, f, fg, bg);
    x = gfx_text_n(x, y, s, max_chars - 2, f, fg, bg);
    return gfx_text(x, y, "..", f, fg, bg);
}

void gfx_text_center(int cx, int y, const char *s, const font_t *f, uint16_t fg, int32_t bg) {
    gfx_text(cx - gfx_text_width(s, f) / 2, y, s, f, fg, bg);
}

void gfx_text_right(int rx, int y, const char *s, const font_t *f, uint16_t fg, int32_t bg) {
    gfx_text(rx - gfx_text_width(s, f), y, s, f, fg, bg);
}

int gfx_printf(int x, int y, const font_t *f, uint16_t fg, int32_t bg, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return gfx_text(x, y, buf, f, fg, bg);
}

uint16_t gfx_contrast(uint16_t bg) {
    int r = (bg >> 11) << 3, g = ((bg >> 5) & 0x3F) << 2, b = (bg & 0x1F) << 3;
    return (r * 299 + g * 587 + b * 114) / 1000 > 140 ? C_BLACK : C_WHITE;
}
