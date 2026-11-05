#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include <stdint.h>
#include <stdbool.h>
#include "ui/gfx.h"
#include "hw/keyboard.h"

// ---- Single line text input (UTF-8 buffer, ASCII typed) ----
typedef struct {
    char buf[232];
    int len;        // bytes
    int cursor;     // byte offset
    int max_bytes;  // limit (<= sizeof(buf)-1)
    int scroll;     // first visible char
} textbox_t;

enum { TB_NONE = 0, TB_CHANGED, TB_ENTER, TB_ESC, TB_UNHANDLED };

void tb_init(textbox_t *tb, int max_bytes, const char *initial);
void tb_clear(textbox_t *tb);
int tb_key(textbox_t *tb, const key_event_t *ev);
void tb_draw(textbox_t *tb, int x, int y, int w, const font_t *f, uint16_t fg, uint16_t bg, bool focused);

// ---- Scrolling list selection ----
typedef struct {
    int sel;
    int top;
    int count;
} listnav_t;

// Handles Up/Down/PgUp/PgDn/Home/End. Returns true if consumed.
bool list_key(listnav_t *l, const key_event_t *ev, int visible);
void list_clamp(listnav_t *l, int visible);

// ---- Word wrap ----
typedef struct {
    uint16_t start; // byte offset
    uint16_t len;   // bytes
} wrap_line_t;

int wrap_text(const char *s, int max_chars, wrap_line_t *lines, int max_lines);

// ---- Decorations ----
// Node "badge": rounded box with the short name in the node's colour.
void draw_node_badge(int x, int y, int w, int h, uint32_t num, const font_t *f);
void draw_scrollbar(int x, int y, int h, int total, int first, int visible);
// Small signal / battery glyphs
void draw_battery(int x, int y, int percent, bool charging);
void draw_section(int y, const char *title); // section header across the content area

#endif
