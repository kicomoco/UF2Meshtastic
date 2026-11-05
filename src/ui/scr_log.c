// F8 - Application / Bluetooth log viewer.
#include <stdio.h>

#include "ui/ui.h"
#include "log.h"

static int offset; // lines scrolled up from the bottom (0 = follow)

static void draw(void) {
    int visible = CONTENT_H / LINE_H;
    int count = app_log_count();
    if (offset > count - visible) offset = count - visible;
    if (offset < 0) offset = 0;
    int first = count - visible - offset;
    if (first < 0) first = 0;
    for (int i = 0; i < visible && first + i < count; i++) {
        const char *line = app_log_line(first + i);
        // Timestamp dimmed, message normal
        int x = gfx_text_n(2, CONTENT_Y + i * LINE_H, line, 9, FONT, C_FAINT, TRANSPARENT);
        const char *rest = line;
        for (int k = 0; k < 9 && *rest; k++) rest++;
        gfx_text_n(x, CONTENT_Y + i * LINE_H, rest, (LCD_W - x - 4) / FONT->w, FONT, C_TEXT, TRANSPARENT);
    }
    draw_scrollbar(LCD_W - 3, CONTENT_Y, CONTENT_H, count, first, visible);
    if (!count) gfx_text_center(LCD_W / 2, 120, "Log is empty", FONT, C_DIM, TRANSPARENT);
}

static bool key(const key_event_t *ev) {
    int visible = CONTENT_H / LINE_H;
    switch (ev->key) {
    case KEY_UP: offset++; break;
    case KEY_DOWN: offset--; break;
    case KEY_PAGE_UP: offset += visible; break;
    case KEY_PAGE_DOWN: offset -= visible; break;
    case KEY_HOME: offset = app_log_count(); break;
    case KEY_END: offset = 0; break;
    default: return false;
    }
    if (offset < 0) offset = 0;
    return true;
}

const screen_t scr_log = {
    .tab_label = "Log",
    .title = "Debug log",
    .draw = draw,
    .key = key,
};
