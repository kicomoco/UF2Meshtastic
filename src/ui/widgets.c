#include "ui/widgets.h"

#include <stdio.h>
#include <string.h>

#include "mesh/mesh.h"

// ------------------------------------------------------------------ textbox

void tb_init(textbox_t *tb, int max_bytes, const char *initial) {
    memset(tb, 0, sizeof(*tb));
    if (max_bytes <= 0 || max_bytes > (int)sizeof(tb->buf) - 1) max_bytes = sizeof(tb->buf) - 1;
    tb->max_bytes = max_bytes;
    if (initial) {
        snprintf(tb->buf, (size_t)max_bytes + 1, "%s", initial);
        tb->len = (int)strlen(tb->buf);
        tb->cursor = tb->len;
    }
}

void tb_clear(textbox_t *tb) {
    tb->buf[0] = 0;
    tb->len = tb->cursor = tb->scroll = 0;
}

static int prev_char(const char *s, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((uint8_t)s[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

static int next_char(const char *s, int pos, int len) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((uint8_t)s[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

int tb_key(textbox_t *tb, const key_event_t *ev) {
    uint8_t k = ev->key;
    switch (k) {
    case KEY_ENTER:
        return TB_ENTER;
    case KEY_ESC:
        return TB_ESC;
    case KEY_LEFT:
        tb->cursor = prev_char(tb->buf, tb->cursor);
        return TB_NONE;
    case KEY_RIGHT:
        tb->cursor = next_char(tb->buf, tb->cursor, tb->len);
        return TB_NONE;
    case KEY_HOME:
        tb->cursor = 0;
        return TB_NONE;
    case KEY_END:
        tb->cursor = tb->len;
        return TB_NONE;
    case KEY_BACKSPACE: {
        if (ev->mods & MOD_CTRL) {
            tb_clear(tb);
            return TB_CHANGED;
        }
        if (tb->cursor == 0) return TB_NONE;
        int p = prev_char(tb->buf, tb->cursor);
        memmove(tb->buf + p, tb->buf + tb->cursor, (size_t)(tb->len - tb->cursor + 1));
        tb->len -= tb->cursor - p;
        tb->cursor = p;
        return TB_CHANGED;
    }
    case KEY_DEL: {
        if (tb->cursor >= tb->len) return TB_NONE;
        int n = next_char(tb->buf, tb->cursor, tb->len);
        memmove(tb->buf + tb->cursor, tb->buf + n, (size_t)(tb->len - n + 1));
        tb->len -= n - tb->cursor;
        return TB_CHANGED;
    }
    default:
        break;
    }
    if (k >= 0x20 && k < 0x7F && !(ev->mods & MOD_CTRL)) {
        if (tb->len + 1 > tb->max_bytes) return TB_NONE;
        memmove(tb->buf + tb->cursor + 1, tb->buf + tb->cursor, (size_t)(tb->len - tb->cursor + 1));
        tb->buf[tb->cursor++] = (char)k;
        tb->len++;
        return TB_CHANGED;
    }
    return TB_UNHANDLED;
}

void tb_draw(textbox_t *tb, int x, int y, int w, const font_t *f, uint16_t fg, uint16_t bg, bool focused) {
    gfx_fill(x, y, w, f->h + 4, bg);
    int cols = (w - 4) / f->w;
    // cursor position in characters
    int cur_chars = 0;
    for (int i = 0; i < tb->cursor;) {
        i = next_char(tb->buf, i, tb->len);
        cur_chars++;
    }
    if (cur_chars < tb->scroll) tb->scroll = cur_chars;
    if (cur_chars >= tb->scroll + cols) tb->scroll = cur_chars - cols + 1;
    const char *s = tb->buf;
    for (int i = 0; i < tb->scroll && *s; i++) utf8_next(&s);
    gfx_text_n(x + 2, y + 2, s, cols, f, fg, TRANSPARENT);
    if (focused) gfx_fill(x + 2 + (cur_chars - tb->scroll) * f->w, y + 1, 1, f->h + 2, C_ACCENT);
}

// ------------------------------------------------------------------ list

void list_clamp(listnav_t *l, int visible) {
    if (l->count <= 0) {
        l->sel = l->top = 0;
        return;
    }
    if (l->sel >= l->count) l->sel = l->count - 1;
    if (l->sel < 0) l->sel = 0;
    if (l->sel < l->top) l->top = l->sel;
    if (l->sel >= l->top + visible) l->top = l->sel - visible + 1;
    if (l->top > l->count - visible) l->top = l->count - visible;
    if (l->top < 0) l->top = 0;
}

bool list_key(listnav_t *l, const key_event_t *ev, int visible) {
    switch (ev->key) {
    case KEY_UP: l->sel--; break;
    case KEY_DOWN: l->sel++; break;
    case KEY_PAGE_UP: l->sel -= visible; break;
    case KEY_PAGE_DOWN: l->sel += visible; break;
    case KEY_HOME: l->sel = 0; break;
    case KEY_END: l->sel = l->count - 1; break;
    default: return false;
    }
    list_clamp(l, visible);
    return true;
}

// ------------------------------------------------------------------ wrap

int wrap_text(const char *s, int max_chars, wrap_line_t *lines, int max_lines) {
    int n = 0;
    const char *base = s;
    if (max_chars < 1) max_chars = 1;
    while (*s && n < max_lines) {
        const char *line = s;
        const char *last_space = NULL;
        const char *p = s;
        int chars = 0;
        while (*p && *p != '\n' && chars < max_chars) {
            if (*p == ' ') last_space = p;
            utf8_next(&p);
            chars++;
        }
        const char *end;
        if (!*p || *p == '\n') {
            end = p;
            s = *p ? p + 1 : p;
        } else if (last_space && last_space > line) {
            end = last_space;
            s = last_space + 1;
        } else {
            end = p;
            s = p;
        }
        lines[n].start = (uint16_t)(line - base);
        lines[n].len = (uint16_t)(end - line);
        n++;
    }
    if (n == 0 && max_lines > 0) {
        lines[0].start = 0;
        lines[0].len = 0;
        n = 1;
    }
    return n;
}

// ------------------------------------------------------------------ decorations

void draw_node_badge(int x, int y, int w, int h, uint32_t num, const font_t *f) {
    uint16_t c = RGB888_TO_565(mesh_node_color(num));
    gfx_round_fill(x, y, w, h, 4, c);
    char label[8];
    mesh_node_label(num, label, sizeof(label));
    int tw = gfx_text_width(label, f);
    if (tw > w - 2) tw = w - 2;
    gfx_text_n(x + (w - tw) / 2, y + (h - f->h) / 2, label, (w - 2) / f->w, f, gfx_contrast(c), TRANSPARENT);
}

void draw_scrollbar(int x, int y, int h, int total, int first, int visible) {
    if (total <= visible || total <= 0) return;
    gfx_fill(x, y, 3, h, C_SURFACE);
    int th = h * visible / total;
    if (th < 8) th = 8;
    int ty = y + (h - th) * first / (total - visible);
    gfx_fill(x, ty, 3, th, C_DIM);
}

void draw_battery(int x, int y, int percent, bool charging) {
    // 18x9 battery outline with nub
    uint16_t col = percent < 0 ? C_DIM : percent <= 20 ? C_RED : percent <= 40 ? C_YELLOW : C_ACCENT;
    gfx_rect(x, y, 16, 9, C_TEXT);
    gfx_fill(x + 16, y + 2, 2, 5, C_TEXT);
    if (percent > 100) percent = 100;
    if (percent > 0) gfx_fill(x + 2, y + 2, 12 * percent / 100 > 0 ? 12 * percent / 100 : 1, 5, col);
    if (charging) {
        gfx_line(x + 9, y + 1, x + 6, y + 4, C_YELLOW);
        gfx_line(x + 6, y + 4, x + 10, y + 4, C_YELLOW);
        gfx_line(x + 10, y + 4, x + 7, y + 7, C_YELLOW);
    }
}

void draw_section(int y, const char *title) {
    gfx_text(6, y, title, &font_6x12, C_ACCENT, TRANSPARENT);
    int w = gfx_text_width(title, &font_6x12);
    gfx_hline(10 + w, y + 6, LCD_W - 16 - w, C_SURFACE2);
}
