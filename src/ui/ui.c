#include "ui/ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pico/stdlib.h"
#include "mesh/mesh.h"
#include "mesh/ble_link.h"
#include "hw/sound.h"
#include "storage.h"
#include "log.h"

static const screen_t *const screens[TAB_COUNT] = {
    &scr_messages, &scr_nodes, &scr_map, &scr_channels, &scr_connect,
    &scr_settings, &scr_stats, &scr_log, &scr_help,     &scr_about,
};

static int cur_tab;
static bool dirty = true;
static uint32_t last_render_ms;
static uint32_t last_second;
static uint32_t seen_change, seen_log;
static int seen_state = -1, seen_scan = -1;
static bool seen_passkey;

static char toast_text[96];
static uint32_t toast_until;

typedef enum { MODAL_NONE, MODAL_PICK, MODAL_PROMPT, MODAL_CONFIRM } modal_t;
static modal_t modal;
static char modal_title[48];
static char modal_text[160];
static const char *const *pick_opts;
static listnav_t pick_nav;
static ui_pick_cb pick_cb;
static textbox_t modal_tb;
static ui_text_cb prompt_cb;
static ui_confirm_cb confirm_cb;

static textbox_t pin_tb;
static bool pin_active;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

void ui_redraw(void) { dirty = true; }
int ui_current_tab(void) { return cur_tab; }

void ui_toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast_text, sizeof(toast_text), fmt, ap);
    va_end(ap);
    toast_until = now_ms() + 3000;
    dirty = true;
}

void ui_goto_tab(int tab) {
    if (tab < 0 || tab >= TAB_COUNT) return;
    cur_tab = tab;
    if (screens[tab]->enter) screens[tab]->enter();
    dirty = true;
}

static void on_new_message(const msg_t *m) {
    uint32_t key = mesh_msg_conv_key(m);
    if (cur_tab == TAB_MESSAGES && messages_is_open(key)) {
        mesh_mark_read(key);
    } else {
        char who[8];
        mesh_node_label(m->from, who, sizeof(who));
        ui_toast("%s: %s", who, m->text);
    }
    if (g_settings.sound) sound_message();
    dirty = true;
}

void ui_init(void) {
    mesh_on_new_message = on_new_message;
    ui_goto_tab(g_settings.has_last_device ? TAB_MESSAGES : TAB_CONNECT);
}

// ------------------------------------------------------------------ modals

bool ui_modal_active(void) { return modal != MODAL_NONE || pin_active; }

void ui_pick(const char *title, const char *const *options, int count, int selected, ui_pick_cb cb) {
    modal = MODAL_PICK;
    snprintf(modal_title, sizeof(modal_title), "%s", title);
    pick_opts = options;
    pick_nav.count = count;
    pick_nav.sel = selected;
    pick_nav.top = 0;
    list_clamp(&pick_nav, 12);
    pick_cb = cb;
    dirty = true;
}

void ui_prompt(const char *title, const char *initial, int max_bytes, ui_text_cb cb) {
    modal = MODAL_PROMPT;
    snprintf(modal_title, sizeof(modal_title), "%s", title);
    tb_init(&modal_tb, max_bytes, initial);
    prompt_cb = cb;
    dirty = true;
}

void ui_confirm(const char *title, const char *text, ui_confirm_cb cb) {
    modal = MODAL_CONFIRM;
    snprintf(modal_title, sizeof(modal_title), "%s", title);
    snprintf(modal_text, sizeof(modal_text), "%s", text);
    confirm_cb = cb;
    dirty = true;
}

static void modal_key(const key_event_t *ev) {
    switch (modal) {
    case MODAL_PICK:
        if (ev->key == KEY_ESC) {
            modal = MODAL_NONE;
        } else if (ev->key == KEY_ENTER) {
            modal = MODAL_NONE;
            if (pick_cb) pick_cb(pick_nav.sel);
        } else {
            list_key(&pick_nav, ev, 12);
        }
        break;
    case MODAL_PROMPT: {
        int r = tb_key(&modal_tb, ev);
        if (r == TB_ESC) {
            modal = MODAL_NONE;
        } else if (r == TB_ENTER) {
            modal = MODAL_NONE;
            if (prompt_cb) prompt_cb(modal_tb.buf);
        }
        break;
    }
    case MODAL_CONFIRM:
        if (ev->key == KEY_ESC || ev->key == 'n' || ev->key == 'N') {
            modal = MODAL_NONE;
        } else if (ev->key == KEY_ENTER || ev->key == 'y' || ev->key == 'Y') {
            modal = MODAL_NONE;
            if (confirm_cb) confirm_cb();
        }
        break;
    default:
        break;
    }
    dirty = true;
}

static void draw_dialog_frame(int x, int y, int w, int h, const char *title) {
    gfx_fill(x + 3, y + 3, w, h, C_BLACK);
    gfx_fill(x, y, w, h, C_SURFACE);
    gfx_rect(x, y, w, h, C_ACCENT);
    gfx_fill(x + 1, y + 1, w - 2, 18, C_ACCENT_DARK);
    gfx_text_fit(x + 6, y + 2, title, w - 12, FONT_BIG, C_TEXT, TRANSPARENT);
}

static void draw_modal(void) {
    switch (modal) {
    case MODAL_PICK: {
        int rows = pick_nav.count < 12 ? pick_nav.count : 12;
        int h = 24 + rows * 14 + 16;
        int x = 30, w = 260, y = (LCD_H - h) / 2;
        draw_dialog_frame(x, y, w, h, modal_title);
        for (int i = 0; i < rows; i++) {
            int idx = pick_nav.top + i;
            int ry = y + 22 + i * 14;
            bool sel = idx == pick_nav.sel;
            if (sel) gfx_fill(x + 2, ry, w - 8, 14, C_ACCENT);
            gfx_text_fit(x + 8, ry + 1, pick_opts[idx], w - 20, FONT, sel ? C_ON_ACCENT : C_TEXT, TRANSPARENT);
        }
        draw_scrollbar(x + w - 5, y + 22, rows * 14, pick_nav.count, pick_nav.top, rows);
        gfx_text(x + 6, y + h - 14, "Enter: select  Esc: cancel", FONT, C_DIM, TRANSPARENT);
        break;
    }
    case MODAL_PROMPT: {
        int x = 20, w = 280, h = 64, y = 110;
        draw_dialog_frame(x, y, w, h, modal_title);
        tb_draw(&modal_tb, x + 6, y + 24, w - 12, FONT, C_TEXT, C_BG, true);
        gfx_printf(x + 6, y + h - 16, FONT, C_DIM, TRANSPARENT, "Enter: save  Esc: cancel  %d/%d", modal_tb.len,
                   modal_tb.max_bytes);
        break;
    }
    case MODAL_CONFIRM: {
        wrap_line_t lines[6];
        int n = wrap_text(modal_text, 40, lines, 6);
        int x = 20, w = 280, h = 24 + n * 12 + 22, y = (LCD_H - h) / 2;
        draw_dialog_frame(x, y, w, h, modal_title);
        for (int i = 0; i < n; i++)
            gfx_text_b(x + 8, y + 24 + i * 12, modal_text + lines[i].start, lines[i].len, FONT, C_TEXT,
                       TRANSPARENT);
        gfx_text(x + 8, y + h - 16, "Enter/Y: confirm   Esc/N: cancel", FONT, C_DIM, TRANSPARENT);
        break;
    }
    default:
        break;
    }
}

static void draw_pin_dialog(void) {
    int x = 20, w = 280, h = 110, y = 90;
    draw_dialog_frame(x, y, w, h, "Bluetooth pairing");
    char name[40];
    snprintf(name, sizeof(name), "%s", ble_link_peer_name()[0] ? ble_link_peer_name() : "the radio");
    gfx_printf(x + 8, y + 24, FONT, C_TEXT, TRANSPARENT, "Enter the PIN shown on");
    gfx_text_fit(x + 8, y + 36, name, w - 16, FONT, C_ACCENT, TRANSPARENT);
    gfx_text(x + 8, y + 48, "(default fixed PIN is 123456)", FONT, C_DIM, TRANSPARENT);
    tb_draw(&pin_tb, x + 80, y + 64, 120, FONT_BIG, C_TEXT, C_BG, true);
    gfx_text(x + 8, y + h - 14, "Enter: pair   Esc: cancel", FONT, C_DIM, TRANSPARENT);
}

static void pin_key(const key_event_t *ev) {
    if (ev->key >= '0' && ev->key <= '9') {
        tb_key(&pin_tb, ev);
    } else if (ev->key == KEY_BACKSPACE || ev->key == KEY_LEFT || ev->key == KEY_RIGHT || ev->key == KEY_DEL) {
        tb_key(&pin_tb, ev);
    } else if (ev->key == KEY_ENTER && pin_tb.len > 0) {
        uint32_t v = 0;
        for (int i = 0; i < pin_tb.len; i++) v = v * 10 + (uint32_t)(pin_tb.buf[i] - '0');
        pin_active = false;
        ble_link_enter_passkey(v);
    } else if (ev->key == KEY_ESC) {
        pin_active = false;
        ble_link_cancel_passkey();
    }
    dirty = true;
}

// ------------------------------------------------------------------ chrome

void ui_format_time(uint32_t epoch, char *out, size_t n) {
    if (!epoch) {
        snprintf(out, n, "--:--");
        return;
    }
    time_t t = (time_t)epoch + g_settings.tz_minutes * 60;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void ui_format_datetime(uint32_t epoch, char *out, size_t n) {
    if (!epoch) {
        snprintf(out, n, "--");
        return;
    }
    time_t t = (time_t)epoch + g_settings.tz_minutes * 60;
    time_t now = (time_t)mesh_now() + g_settings.tz_minutes * 60;
    struct tm tm, tn;
    gmtime_r(&t, &tm);
    gmtime_r(&now, &tn);
    if (tm.tm_yday == tn.tm_yday && tm.tm_year == tn.tm_year) {
        snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        snprintf(out, n, "%02d/%02d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
    }
}

void ui_format_distance(float m, char *out, size_t n) {
    if (g_settings.imperial) {
        float ft = m * 3.28084f;
        if (ft < 1000) snprintf(out, n, "%.0f ft", ft);
        else snprintf(out, n, "%.1f mi", m / 1609.344f);
    } else {
        if (m < 1000) snprintf(out, n, "%.0f m", m);
        else if (m < 100000) snprintf(out, n, "%.1f km", m / 1000);
        else snprintf(out, n, "%.0f km", m / 1000);
    }
}

static void draw_bt_icon(int x, int y, uint16_t c) {
    // Bluetooth rune, 7x12
    gfx_vline(x + 3, y, 12, c);
    gfx_line(x + 3, y, x + 6, y + 3, c);
    gfx_line(x + 6, y + 3, x, y + 8, c);
    gfx_line(x + 3, y + 11, x + 6, y + 8, c);
    gfx_line(x + 6, y + 8, x, y + 3, c);
}

static void draw_status_bar(void) {
    gfx_fill(0, 0, LCD_W, STATUS_H, C_BAR);
    const screen_t *s = screens[cur_tab];
    const char *title = s->dyn_title ? s->dyn_title() : NULL;
    if (!title) title = s->title;

    int rx = LCD_W - 2;
    // PicoCalc battery
    bool chg = false;
    int pct = kbd_battery_percent(&chg);
    if (pct >= 0) {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", pct);
        rx -= gfx_text_width(b, FONT);
        gfx_text(rx, 2, b, FONT, C_TEXT, TRANSPARENT);
        rx -= 21;
        draw_battery(rx, 4, pct, chg);
        rx -= 6;
    }
    char clock[8];
    ui_format_time(mesh_now(), clock, sizeof(clock));
    rx -= gfx_text_width(clock, FONT);
    gfx_text(rx, 2, clock, FONT, C_TEXT, TRANSPARENT);
    rx -= 6;

    if (g_mesh.config_complete) {
        char nodes[16];
        snprintf(nodes, sizeof(nodes), "%d/%d", mesh_nodes_online(), g_mesh.node_count > 0 ? g_mesh.node_count - 1 : 0);
        rx -= gfx_text_width(nodes, FONT);
        gfx_text(rx, 2, nodes, FONT, C_DIM, TRANSPARENT);
        rx -= 6;
    }

    link_state_t st = ble_link_state();
    uint16_t bc = st == LINK_CONNECTED ? (g_mesh.config_complete ? C_ACCENT : C_YELLOW)
                  : st == LINK_IDLE || st == LINK_OFF ? C_RED
                                                      : C_YELLOW;
    rx -= 9;
    draw_bt_icon(rx, 2, bc);
    rx -= 4;

    gfx_text_fit(4, 0, title, rx - 6, FONT_BIG, C_TEXT, TRANSPARENT);
}

static void draw_tab_bar(void) {
    int y = LCD_H - TABBAR_H;
    gfx_fill(0, y, LCD_W, TABBAR_H, C_BAR);
    gfx_hline(0, y, LCD_W, C_SURFACE2);
    int unread = mesh_total_unread();
    for (int i = 0; i < TAB_COUNT; i++) {
        int x = i * 32;
        bool sel = i == cur_tab;
        if (sel) gfx_fill(x + 1, y + 1, 30, TABBAR_H - 1, C_ACCENT);
        gfx_text_center(x + 16, y + 3, screens[i]->tab_label, FONT, sel ? C_ON_ACCENT : C_DIM, TRANSPARENT);
        if (i == TAB_MESSAGES && unread) gfx_fill_circle(x + 28, y + 4, 3, C_RED);
    }
}

static void draw_toast(void) {
    if (!toast_text[0] || (int32_t)(now_ms() - toast_until) >= 0) {
        toast_text[0] = 0;
        return;
    }
    int w = gfx_text_width(toast_text, FONT) + 16;
    if (w > LCD_W - 16) w = LCD_W - 16;
    int x = (LCD_W - w) / 2, y = CONTENT_BOTTOM - 26;
    gfx_round_fill(x, y, w, 20, 6, C_SURFACE2);
    gfx_text_fit(x + 8, y + 4, toast_text, w - 16, FONT, C_TEXT, TRANSPARENT);
}

// ------------------------------------------------------------------ input / render

void ui_key(const key_event_t *ev) {
    dirty = true;
    if (pin_active) {
        pin_key(ev);
        return;
    }
    if (modal != MODAL_NONE) {
        modal_key(ev);
        return;
    }
    uint8_t k = ev->key;
    if (k >= KEY_F1 && k <= KEY_F9) {
        ui_goto_tab(k - KEY_F1);
        return;
    }
    if (k == KEY_F10) {
        ui_goto_tab(TAB_ABOUT);
        return;
    }
    // Ctrl+Left/Right cycles tabs as an alternative to the function keys
    if ((ev->mods & MOD_CTRL) && (k == KEY_LEFT || k == KEY_RIGHT)) {
        ui_goto_tab((cur_tab + (k == KEY_RIGHT ? 1 : TAB_COUNT - 1)) % TAB_COUNT);
        return;
    }
    if (screens[cur_tab]->key) screens[cur_tab]->key(ev);
}

bool ui_render(void) {
    uint32_t now = now_ms();
    bool pk = ble_link_needs_passkey();
    if (pk && !seen_passkey) {
        tb_init(&pin_tb, 6, NULL);
        pin_active = true;
        if (g_settings.sound) sound_alert();
    } else if (!pk) {
        pin_active = false;
    }
    if (pk != seen_passkey) dirty = true;
    seen_passkey = pk;

    if (g_mesh.change_counter != seen_change) {
        seen_change = g_mesh.change_counter;
        dirty = true;
    }
    if ((int)ble_link_state() != seen_state || ble_link_scan_count() != seen_scan) {
        seen_state = (int)ble_link_state();
        seen_scan = ble_link_scan_count();
        dirty = true;
    }
    if (cur_tab == TAB_LOG && app_log_serial() != seen_log) {
        seen_log = app_log_serial();
        dirty = true;
    }
    if (now / 1000 != last_second) {
        last_second = now / 1000;
        dirty = true;
    }
    if (!dirty || now - last_render_ms < 33) return false;
    dirty = false;
    last_render_ms = now;

    gfx_clip_reset();
    gfx_fill(0, CONTENT_Y, LCD_W, CONTENT_H, C_BG);
    gfx_clip(0, CONTENT_Y, LCD_W, CONTENT_H);
    screens[cur_tab]->draw();
    gfx_clip_reset();
    draw_status_bar();
    draw_tab_bar();
    draw_toast();
    if (modal != MODAL_NONE) draw_modal();
    if (pin_active) draw_pin_dialog();
    return true;
}
