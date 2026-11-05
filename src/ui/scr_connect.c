// F5 - Bluetooth connection management (scan, connect, pairing).
#include <stdio.h>
#include <string.h>

#include "ui/ui.h"
#include "mesh/mesh.h"
#include "mesh/ble_link.h"
#include "storage.h"
#include "btstack_util.h"

#define ROW_H 26

static listnav_t nav;
static int order[SCAN_MAX];
static int order_count;
static bool show_all;

static void rebuild(void) {
    order_count = 0;
    // Meshtastic devices first, sorted by RSSI
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < ble_link_scan_count(); i++) {
            const scan_result_t *r = ble_link_scan_result(i);
            if ((pass == 0) != r->meshtastic) continue;
            if (!r->meshtastic && !show_all) continue;
            int j = order_count++;
            while (j > 0) {
                const scan_result_t *p = ble_link_scan_result(order[j - 1]);
                if (p->meshtastic != r->meshtastic || p->rssi >= r->rssi) break;
                order[j] = order[j - 1];
                j--;
            }
            order[j] = i;
        }
    }
    nav.count = order_count;
}

static void enter(void) {
    if (ble_link_state() == LINK_IDLE && !g_settings.has_last_device && ble_link_scan_count() == 0) ble_link_start_scan();
}

static void signal_bars(int x, int y, int rssi) {
    int bars = rssi > -60 ? 4 : rssi > -70 ? 3 : rssi > -80 ? 2 : 1;
    for (int b = 0; b < 4; b++) {
        int h = 3 + b * 3;
        gfx_fill(x + b * 4, y + 12 - h, 3, h, b < bars ? C_ACCENT : C_SURFACE2);
    }
}

static void draw(void) {
    rebuild();
    int y = CONTENT_Y + 4;
    link_state_t st = ble_link_state();

    // Current device card
    gfx_round_fill(4, y, LCD_W - 8, 58, 6, C_SURFACE);
    uint16_t sc = st == LINK_CONNECTED ? C_ACCENT : (st == LINK_IDLE || st == LINK_OFF) ? C_RED : C_YELLOW;
    gfx_fill_circle(16, y + 10, 4, sc);
    const char *state = ble_link_state_name();
    if (st == LINK_CONNECTED && !g_mesh.config_complete) state = "Downloading node database";
    gfx_text(26, y + 3, state, FONT_BIG, C_TEXT, TRANSPARENT);

    const char *name = ble_link_peer_name();
    if (!name[0] && g_settings.has_last_device) name = g_settings.last_name;
    char line[64];
    if (st != LINK_IDLE && st != LINK_OFF && st != LINK_SCANNING) {
        snprintf(line, sizeof(line), "%s  %s", name[0] ? name : "Radio", bd_addr_to_str(ble_link_peer_addr()));
    } else if (g_settings.has_last_device) {
        snprintf(line, sizeof(line), "Last: %s  %s", g_settings.last_name[0] ? g_settings.last_name : "radio",
                 bd_addr_to_str(g_settings.last_addr));
    } else {
        snprintf(line, sizeof(line), "No radio paired yet");
    }
    gfx_text_fit(12, y + 22, line, LCD_W - 24, FONT, C_DIM, TRANSPARENT);
    if (st == LINK_CONNECTED) {
        const node_t *me = mesh_my_node();
        snprintf(line, sizeof(line), "%s%s  fw %s  MTU %u", me ? me->long_name : "", me ? "" : "",
                 g_mesh.has_metadata ? g_mesh.metadata.firmware_version : "?", ble_link_mtu());
        gfx_text_fit(12, y + 36, line, LCD_W - 24, FONT, C_TEXT, TRANSPARENT);
    } else if (ble_link_last_error()[0]) {
        gfx_text_fit(12, y + 36, ble_link_last_error(), LCD_W - 24, FONT, C_RED, TRANSPARENT);
    }
    y += 64;

    char hdr[48];
    snprintf(hdr, sizeof(hdr), "%s  (auto-connect %s)", ble_link_scanning() ? "Scanning..." : "Devices",
             g_settings.auto_connect ? "on" : "off");
    draw_section(y, hdr);
    y += 14;

    int visible = (CONTENT_BOTTOM - 28 - y) / ROW_H;
    list_clamp(&nav, visible);
    if (!order_count) {
        gfx_text_center(LCD_W / 2, y + 30,
                        ble_link_scanning() ? "Looking for Meshtastic radios..." : "Press S to scan for radios", FONT,
                        C_DIM, TRANSPARENT);
        gfx_text_center(LCD_W / 2, y + 46, "Make sure Bluetooth is enabled on the radio", FONT, C_FAINT, TRANSPARENT);
    }
    for (int i = 0; i < visible && nav.top + i < order_count; i++) {
        int idx = nav.top + i;
        const scan_result_t *r = ble_link_scan_result(order[idx]);
        int ry = y + i * ROW_H;
        bool sel = idx == nav.sel;
        if (sel) gfx_fill(0, ry, LCD_W, ROW_H, C_SURFACE2);
        signal_bars(8, ry + 6, r->rssi);
        bool is_last = g_settings.has_last_device && memcmp(r->addr, g_settings.last_addr, 6) == 0;
        gfx_text_fit(30, ry + 1, r->name[0] ? r->name : "(unnamed)", LCD_W - 100, FONT,
                     r->meshtastic ? C_TEXT : C_DIM, TRANSPARENT);
        gfx_printf(30, ry + 13, FONT, C_FAINT, TRANSPARENT, "%s  %ddBm%s", bd_addr_to_str(r->addr), r->rssi,
                   is_last ? "  (last used)" : "");
        if (r->meshtastic) gfx_text_right(LCD_W - 6, ry + 1, "Meshtastic", FONT, C_ACCENT, TRANSPARENT);
    }
    draw_scrollbar(LCD_W - 3, y, visible * ROW_H, order_count, nav.top, visible);

    gfx_text(4, CONTENT_BOTTOM - 26, "Enter: connect  S: scan  D: disconnect", FONT, C_FAINT, TRANSPARENT);
    gfx_text(4, CONTENT_BOTTOM - 14, "A: auto-connect  O: show all  X: forget pairs", FONT, C_FAINT, TRANSPARENT);
}

static void do_forget(void) {
    ble_link_disconnect();
    ble_link_forget_bonds();
    ui_toast("Pairings forgotten");
}

static bool key(const key_event_t *ev) {
    rebuild();
    int visible = 6;
    if (list_key(&nav, ev, visible)) return true;
    switch (ev->key) {
    case KEY_ENTER:
        if (order_count) {
            const scan_result_t *r = ble_link_scan_result(order[nav.sel]);
            link_state_t st = ble_link_state();
            if (st != LINK_IDLE && st != LINK_SCANNING) {
                ui_toast("Disconnect first (D)");
            } else {
                ble_link_connect(r->addr, r->addr_type, r->name);
            }
        } else if (g_settings.has_last_device && ble_link_state() == LINK_IDLE) {
            ble_link_connect(g_settings.last_addr, g_settings.last_addr_type, g_settings.last_name);
        }
        return true;
    case 's': case 'S':
        if (ble_link_scanning()) ble_link_stop_scan();
        else if (ble_link_state() == LINK_IDLE) ble_link_start_scan();
        else ui_toast("Disconnect before scanning");
        return true;
    case 'd': case 'D':
        ble_link_disconnect();
        return true;
    case 'a': case 'A':
        g_settings.auto_connect = !g_settings.auto_connect;
        settings_save();
        ui_toast("Auto-connect %s", g_settings.auto_connect ? "on" : "off");
        return true;
    case 'o': case 'O':
        show_all = !show_all;
        return true;
    case 'x': case 'X':
        ui_confirm("Forget pairings", "Disconnect and delete all stored Bluetooth pairings?", do_forget);
        return true;
    }
    return false;
}

const screen_t scr_connect = {
    .tab_label = "Conn",
    .title = "Connection",
    .enter = enter,
    .draw = draw,
    .key = key,
};
