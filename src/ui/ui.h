#ifndef UI_UI_H
#define UI_UI_H

#include <stdint.h>
#include <stdbool.h>
#include "ui/gfx.h"
#include "ui/widgets.h"
#include "hw/keyboard.h"

#define STATUS_H 16
#define TABBAR_H 16
#define CONTENT_Y STATUS_H
#define CONTENT_H (LCD_H - STATUS_H - TABBAR_H)
#define CONTENT_BOTTOM (CONTENT_Y + CONTENT_H)

#define FONT (&font_6x12)
#define FONT_BIG (&font_8x16)
#define LINE_H 12

typedef struct {
    const char *tab_label; // max 5 chars, shown in the tab bar
    const char *title;     // shown in the status bar
    void (*enter)(void);   // called when the tab becomes active
    void (*draw)(void);
    // Return true if the key was consumed. Esc not consumed by a screen is ignored.
    bool (*key)(const key_event_t *ev);
    // Optional dynamic title (e.g. chat name); NULL to use `title`.
    const char *(*dyn_title)(void);
} screen_t;

enum {
    TAB_MESSAGES = 0,
    TAB_NODES,
    TAB_MAP,
    TAB_CHANNELS,
    TAB_CONNECT,
    TAB_SETTINGS,
    TAB_STATS,
    TAB_LOG,
    TAB_HELP,
    TAB_ABOUT,
    TAB_COUNT
};

extern const screen_t scr_messages, scr_nodes, scr_map, scr_channels, scr_connect, scr_settings, scr_stats, scr_log,
    scr_help, scr_about;

void ui_init(void);
void ui_key(const key_event_t *ev);
// Renders if anything changed; returns true if the framebuffer was updated.
bool ui_render(void);
void ui_redraw(void);

void ui_goto_tab(int tab);
int ui_current_tab(void);
void ui_toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Messages screen entry point used by other tabs.
void messages_open_conversation(uint32_t conv_key);
bool messages_is_open(uint32_t conv_key);

// Map / node detail cross-links
void nodes_open_detail(uint32_t num);
void map_focus_node(uint32_t num);

// ---- Modal dialogs (only one at a time) ----
typedef void (*ui_pick_cb)(int choice);
typedef void (*ui_text_cb)(const char *text);
typedef void (*ui_confirm_cb)(void);

void ui_pick(const char *title, const char *const *options, int count, int selected, ui_pick_cb cb);
void ui_prompt(const char *title, const char *initial, int max_bytes, ui_text_cb cb);
void ui_confirm(const char *title, const char *text, ui_confirm_cb cb);
bool ui_modal_active(void);

// Time formatting using the radio's clock and the configured timezone.
void ui_format_time(uint32_t epoch, char *out, size_t n);      // HH:MM
void ui_format_datetime(uint32_t epoch, char *out, size_t n);  // dd/mm HH:MM
void ui_format_distance(float metres, char *out, size_t n);

#endif
