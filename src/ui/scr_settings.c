// F6 - Radio and app settings.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ui.h"
#include "mesh/mesh.h"
#include "storage.h"
#include "hw/keyboard.h"
#include "map/tiles.h"
#include "net/wifi.h"

// The keyboard controller treats values above 240 as "off" for the keyboard light.
#define BL_MAX 240

enum {
    S_HDR_RADIO,
    S_LONG,
    S_SHORT,
    S_ROLE,
    S_REGION,
    S_PRESET,
    S_HOPS,
    S_REBOOT,
    S_HDR_APP,
    S_SOUND,
    S_AUTOCONN,
    S_UNITS,
    S_LCD_BL,
    S_KBD_BL,
    S_TZ,
    S_CLEAR,
    S_HDR_NET,
    S_WIFI,
    S_WIFI_NET,
    S_WIFI_STATUS,
    S_WIFI_IP,
    S_MAP_SRC,
    S_MAP_CLEAR,
    S_COUNT
};

#define ROW_H 16

static listnav_t nav = {.sel = S_LONG};
static const char *opt_ptrs[40];
static char opt_buf[40][16];

static bool is_header(int i) { return i == S_HDR_RADIO || i == S_HDR_APP || i == S_HDR_NET; }

static bool scan_pending; // show the network picker when the scan finishes

static bool need_radio(void) {
    if (g_mesh.link_up && g_mesh.config_complete) return true;
    ui_toast("Connect to a radio first");
    return false;
}

static void label_value(int i, char *label, char *val, size_t n) {
    const node_t *me = mesh_my_node();
    val[0] = 0;
    switch (i) {
    case S_HDR_RADIO: snprintf(label, n, "Radio"); break;
    case S_LONG:
        snprintf(label, n, "Long name");
        snprintf(val, n, "%s", me ? me->long_name : "-");
        break;
    case S_SHORT:
        snprintf(label, n, "Short name");
        snprintf(val, n, "%s", me ? me->short_name : "-");
        break;
    case S_ROLE:
        snprintf(label, n, "Device role");
        snprintf(val, n, "%s", g_mesh.has_device ? mesh_role_name(g_mesh.device.role) : "-");
        break;
    case S_REGION:
        snprintf(label, n, "LoRa region");
        snprintf(val, n, "%s", g_mesh.has_lora ? mesh_region_name(g_mesh.lora.region) : "-");
        break;
    case S_PRESET:
        snprintf(label, n, "Modem preset");
        snprintf(val, n, "%s",
                 g_mesh.has_lora ? (g_mesh.lora.use_preset ? mesh_preset_name(g_mesh.lora.modem_preset) : "Custom") : "-");
        break;
    case S_HOPS:
        snprintf(label, n, "Hop limit");
        if (g_mesh.has_lora) snprintf(val, n, "%lu", (unsigned long)g_mesh.lora.hop_limit);
        break;
    case S_REBOOT: snprintf(label, n, "Reboot radio"); break;
    case S_HDR_APP: snprintf(label, n, "PicoCalc app"); break;
    case S_SOUND:
        snprintf(label, n, "Notification sound");
        snprintf(val, n, "%s", g_settings.sound ? "On" : "Off");
        break;
    case S_AUTOCONN:
        snprintf(label, n, "Auto-connect");
        snprintf(val, n, "%s", g_settings.auto_connect ? "On" : "Off");
        break;
    case S_UNITS:
        snprintf(label, n, "Units");
        snprintf(val, n, "%s", g_settings.imperial ? "Imperial" : "Metric");
        break;
    case S_LCD_BL:
        snprintf(label, n, "Screen brightness");
        snprintf(val, n, "%d%%", (g_settings.lcd_backlight * 100 + BL_MAX / 2) / BL_MAX);
        break;
    case S_KBD_BL:
        snprintf(label, n, "Keyboard backlight");
        if (kbd_has_kbd_backlight())
            snprintf(val, n, "%d%%", (g_settings.kbd_backlight * 100 + BL_MAX / 2) / BL_MAX);
        else
            snprintf(val, n, "n/a");
        break;
    case S_TZ: {
        snprintf(label, n, "Time zone (UTC offset)");
        int m = g_settings.tz_minutes;
        snprintf(val, n, "%c%d:%02d", m < 0 ? '-' : '+', abs(m) / 60, abs(m) % 60);
        break;
    }
    case S_CLEAR: snprintf(label, n, "Clear message history"); break;
    case S_HDR_NET: snprintf(label, n, "Wi-Fi & map"); break;
    case S_WIFI:
        snprintf(label, n, "Wi-Fi");
        snprintf(val, n, "%s", g_settings.wifi_enabled ? "On" : "Off");
        break;
    case S_WIFI_NET:
        snprintf(label, n, "Network");
        if (scan_pending) snprintf(val, n, "Scanning...");
        else snprintf(val, n, "%.24s", g_settings.wifi_ssid[0] ? g_settings.wifi_ssid : "Choose...");
        break;
    case S_WIFI_STATUS:
        snprintf(label, n, "Status");
        if (wifi_state() == WIFI_CONNECTED) snprintf(val, n, "%s %ddBm", wifi_status_text(), wifi_rssi());
        else snprintf(val, n, "%s", wifi_status_text());
        break;
    case S_WIFI_IP:
        snprintf(label, n, "IP address");
        snprintf(val, n, "%s", wifi_ip_text());
        break;
    case S_MAP_SRC: {
        snprintf(label, n, "Map tiles");
        const tile_source_t *src = tiles_active_source();
        snprintf(val, n, "%s", src ? src->name : "Off");
        break;
    }
    case S_MAP_CLEAR: snprintf(label, n, "Clear map tile cache"); break;
    }
}

// --- callbacks ---

static void cb_long(const char *t) {
    const node_t *me = mesh_my_node();
    if (t[0] && mesh_admin_set_owner(t, me ? me->short_name : "")) ui_toast("Name updated");
}

static void cb_short(const char *t) {
    const node_t *me = mesh_my_node();
    if (t[0] && mesh_admin_set_owner(me ? me->long_name : t, t)) ui_toast("Short name updated");
}

static void cb_role(int c) {
    meshtastic_Config_DeviceConfig d = g_mesh.device;
    d.role = (meshtastic_Config_DeviceConfig_Role)c;
    if (mesh_admin_set_device(&d)) ui_toast("Role set, radio will reboot");
}

static void cb_region(int c) {
    meshtastic_Config_LoRaConfig l = g_mesh.lora;
    l.region = (meshtastic_Config_LoRaConfig_RegionCode)c;
    if (mesh_admin_set_lora(&l)) ui_toast("Region set, radio will reboot");
}

static void cb_preset(int c) {
    meshtastic_Config_LoRaConfig l = g_mesh.lora;
    l.use_preset = true;
    l.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)c;
    if (mesh_admin_set_lora(&l)) ui_toast("Preset set, radio will reboot");
}

static void cb_hops(int c) {
    meshtastic_Config_LoRaConfig l = g_mesh.lora;
    l.hop_limit = (uint32_t)(c + 1);
    if (mesh_admin_set_lora(&l)) ui_toast("Hop limit set");
}

static void cb_reboot(void) {
    if (mesh_admin_reboot(2)) ui_toast("Radio rebooting");
}

static const uint8_t bl_levels[] = {0, 30, 60, 120, 180, BL_MAX};

static void cb_lcd(int c) {
    g_settings.lcd_backlight = bl_levels[c + 1];
    kbd_set_lcd_backlight(g_settings.lcd_backlight);
    settings_save();
}

static void cb_kbd(int c) {
    g_settings.kbd_backlight = bl_levels[c];
    kbd_set_kbd_backlight(g_settings.kbd_backlight);
    settings_save();
}

static void cb_tz(const char *t) {
    int sign = 1;
    if (*t == '-') { sign = -1; t++; }
    else if (*t == '+') t++;
    int h = atoi(t);
    int m = 0;
    const char *colon = strchr(t, ':');
    if (colon) m = atoi(colon + 1);
    int total = sign * (h * 60 + m);
    if (total < -12 * 60 || total > 14 * 60) {
        ui_toast("Offset must be -12:00 to +14:00");
        return;
    }
    g_settings.tz_minutes = (int16_t)total;
    settings_save();
}

static void cb_clear(void) {
    mesh_clear_messages();
    ui_toast("Message history cleared");
}

// --- Wi-Fi ---

static char new_ssid[33];

static void wifi_save(const char *ssid, const char *pass) {
    snprintf(g_settings.wifi_ssid, sizeof(g_settings.wifi_ssid), "%s", ssid);
    snprintf(g_settings.wifi_pass, sizeof(g_settings.wifi_pass), "%s", pass);
    g_settings.wifi_enabled = true;
    settings_save();
    wifi_apply_settings();
    ui_toast("Connecting to %s", ssid);
}

static void cb_wifi_pass(const char *t) { wifi_save(new_ssid, t); }

static void ask_password(void) {
    bool same = strcmp(new_ssid, g_settings.wifi_ssid) == 0;
    ui_prompt("Wi-Fi password", same ? g_settings.wifi_pass : "", 64, cb_wifi_pass);
}

static void cb_wifi_ssid(const char *t) {
    if (!t[0]) return;
    snprintf(new_ssid, sizeof(new_ssid), "%s", t);
    ask_password();
}

static void cb_wifi_pick(int c) {
    const wifi_ap_t *ap = wifi_scan_result(c);
    if (!ap) {
        ui_prompt("Network name (SSID)", "", 32, cb_wifi_ssid);
        return;
    }
    snprintf(new_ssid, sizeof(new_ssid), "%s", ap->ssid);
    if (ap->secure) ask_password();
    else wifi_save(new_ssid, "");
}

static void show_networks(void) {
    int n = wifi_scan_count();
    if (n > 39) n = 39;
    int sel = 0;
    for (int i = 0; i < n; i++) {
        const wifi_ap_t *ap = wifi_scan_result(i);
        snprintf(opt_buf[i], sizeof(opt_buf[i]), "%s", ap->ssid);
        opt_ptrs[i] = opt_buf[i];
        if (strcmp(ap->ssid, g_settings.wifi_ssid) == 0) sel = i;
    }
    opt_ptrs[n] = "Other...";
    ui_pick(n ? "Wi-Fi network" : "No networks found", opt_ptrs, n + 1, sel, cb_wifi_pick);
}

static void cb_map_src(int c) {
    g_settings.map_source = (uint8_t)c;
    settings_save();
}

static void cb_map_clear(void) {
    int n = tiles_clear_cache();
    ui_toast("Deleted %d cached tiles", n);
}

static void pick_names(const char *title, const char *(*namefn)(int), int count, int sel, ui_pick_cb cb) {
    for (int i = 0; i < count && i < 40; i++) {
        snprintf(opt_buf[i], sizeof(opt_buf[i]), "%s", namefn(i));
        opt_ptrs[i] = opt_buf[i];
    }
    ui_pick(title, opt_ptrs, count, sel, cb);
}

static void activate(int i) {
    const node_t *me = mesh_my_node();
    switch (i) {
    case S_LONG:
        if (need_radio()) ui_prompt("Long name", me ? me->long_name : "", 39, cb_long);
        break;
    case S_SHORT:
        if (need_radio()) ui_prompt("Short name (max 4)", me ? me->short_name : "", 4, cb_short);
        break;
    case S_ROLE:
        if (need_radio()) pick_names("Device role", mesh_role_name, 13, g_mesh.device.role, cb_role);
        break;
    case S_REGION:
        if (need_radio()) pick_names("LoRa region", mesh_region_name, 27, g_mesh.lora.region, cb_region);
        break;
    case S_PRESET:
        if (need_radio()) pick_names("Modem preset", mesh_preset_name, 17, g_mesh.lora.modem_preset, cb_preset);
        break;
    case S_HOPS:
        if (need_radio()) {
            static const char *const hops[] = {"1", "2", "3", "4", "5", "6", "7"};
            int cur = g_mesh.lora.hop_limit ? (int)g_mesh.lora.hop_limit - 1 : 2;
            ui_pick("Hop limit", hops, 7, cur, cb_hops);
        }
        break;
    case S_REBOOT:
        if (need_radio()) ui_confirm("Reboot radio", "Reboot the connected radio now?", cb_reboot);
        break;
    case S_SOUND:
        g_settings.sound = !g_settings.sound;
        settings_save();
        break;
    case S_AUTOCONN:
        g_settings.auto_connect = !g_settings.auto_connect;
        settings_save();
        break;
    case S_UNITS:
        g_settings.imperial = !g_settings.imperial;
        settings_save();
        break;
    case S_LCD_BL: {
        static const char *const lv[] = {"12%", "25%", "50%", "75%", "100%"};
        int cur = 0;
        for (int k = 1; k < 6; k++) if (g_settings.lcd_backlight >= bl_levels[k]) cur = k - 1;
        ui_pick("Screen brightness", lv, 5, cur, cb_lcd);
        break;
    }
    case S_KBD_BL: {
        if (!kbd_has_kbd_backlight()) {
            ui_toast("Needs newer keyboard firmware");
            break;
        }
        static const char *const lv[] = {"Off", "12%", "25%", "50%", "75%", "100%"};
        int cur = 0;
        for (int k = 0; k < 6; k++) if (g_settings.kbd_backlight >= bl_levels[k]) cur = k;
        ui_pick("Keyboard backlight", lv, 6, cur, cb_kbd);
        break;
    }
    case S_TZ: {
        char cur[12];
        int m = g_settings.tz_minutes;
        snprintf(cur, sizeof(cur), "%c%d:%02d", m < 0 ? '-' : '+', abs(m) / 60, abs(m) % 60);
        ui_prompt("UTC offset, e.g. +1 or -5:30", cur, 6, cb_tz);
        break;
    }
    case S_CLEAR:
        ui_confirm("Clear history", "Delete all stored messages on this PicoCalc?", cb_clear);
        break;
    case S_WIFI:
        g_settings.wifi_enabled = !g_settings.wifi_enabled;
        settings_save();
        wifi_apply_settings();
        break;
    case S_WIFI_NET:
        if (wifi_scan_start()) scan_pending = true;
        else ui_toast("Wi-Fi scan failed");
        break;
    case S_WIFI_STATUS:
        if (!g_settings.wifi_enabled) ui_toast("Wi-Fi is off");
        else if (!g_settings.wifi_ssid[0]) ui_toast("Choose a network first");
        else wifi_apply_settings();
        break;
    case S_MAP_SRC: {
        static const char *srcs[8];
        int n = tiles_source_count();
        srcs[0] = "Off (plain map)";
        for (int k = 0; k < n && k < 7; k++) srcs[k + 1] = tiles_source(k)->name;
        ui_pick("Map tiles", srcs, n + 1, g_settings.map_source, cb_map_src);
        break;
    }
    case S_MAP_CLEAR:
        ui_confirm("Clear map cache", "Delete all downloaded map tiles from the SD card?", cb_map_clear);
        break;
    }
}

static int visible_rows(void) {
    // The last row is taken by the "needs a radio" note when disconnected.
    return CONTENT_H / ROW_H - (g_mesh.link_up && g_mesh.config_complete ? 0 : 1);
}

static void draw(void) {
    if (scan_pending && !wifi_scan_active()) {
        scan_pending = false;
        if (!ui_modal_active()) show_networks();
    }
    nav.count = S_COUNT;
    int visible = visible_rows();
    list_clamp(&nav, visible);
    char label[48], val[48];
    for (int r = 0; r < visible && nav.top + r < S_COUNT; r++) {
        int i = nav.top + r;
        int y = CONTENT_Y + r * ROW_H;
        label_value(i, label, val, sizeof(label));
        if (is_header(i)) {
            draw_section(y + 3, label);
            continue;
        }
        bool sel = i == nav.sel;
        if (sel) gfx_fill(0, y, LCD_W, ROW_H, C_SURFACE);
        bool radio = i < S_HDR_APP;
        bool enabled = !radio || (g_mesh.link_up && g_mesh.config_complete);
        gfx_text(12, y + 2, label, FONT, enabled ? C_TEXT : C_FAINT, TRANSPARENT);
        gfx_text_right(LCD_W - 8, y + 2, val, FONT, enabled ? C_ACCENT : C_FAINT, TRANSPARENT);
    }
    if (!(g_mesh.link_up && g_mesh.config_complete)) {
        gfx_text(12, CONTENT_BOTTOM - 14, "Radio settings need a connected radio.", FONT, C_FAINT, TRANSPARENT);
    }
}

static bool key(const key_event_t *ev) {
    nav.count = S_COUNT;
    int old = nav.sel;
    if (list_key(&nav, ev, visible_rows())) {
        // skip headers
        if (is_header(nav.sel)) {
            nav.sel += nav.sel >= old ? 1 : -1;
            if (nav.sel < 0) nav.sel = S_LONG;
            list_clamp(&nav, visible_rows());
        }
        return true;
    }
    if (ev->key == KEY_ENTER || ev->key == ' ') {
        activate(nav.sel);
        return true;
    }
    return false;
}

const screen_t scr_settings = {
    .tab_label = "Set",
    .title = "Settings",
    .draw = draw,
    .key = key,
};
