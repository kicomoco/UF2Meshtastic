// F9 - Help, F10 - About.
#include <stdio.h>

#include "ui/ui.h"
#include "mesh/mesh.h"

static const char *const help_lines[] = {
    "#Tabs",
    "F1 Chat     conversations and messages",
    "F2 Nodes    mesh nodes, type to filter",
    "F3 Map      node positions (offline)",
    "F4 Chan     channels, LoRa, share URL",
    "F5 Conn     Bluetooth scan / connect",
    "F6 Set      radio and app settings",
    "F7 Stat     statistics",
    "F8 Log      debug log",
    "F9 Help     this page    F10 About",
    "Ctrl+Left/Right also switches tabs",
    "#Everywhere",
    "Up/Down PgUp/PgDn  move / scroll",
    "Enter       open / select / send",
    "Esc         back / cancel",
    "#Chat",
    "Type and press Enter to send",
    "Shift+Esc   clear the message being typed",
    "#Map",
    "Arrows pan, +/- zoom, Tab select node",
    "C centre on me, F fit all, Enter details",
    "#First connection",
    "F5, S to scan, select the radio, Enter.",
    "Type the PIN shown on the radio's screen",
    "(fixed-PIN radios default to 123456).",
};

static int scroll;

static void draw_help(void) {
    int n = (int)(sizeof(help_lines) / sizeof(help_lines[0]));
    int visible = CONTENT_H / 13;
    if (scroll > n - visible) scroll = n - visible;
    if (scroll < 0) scroll = 0;
    for (int i = 0; i < visible && scroll + i < n; i++) {
        const char *l = help_lines[scroll + i];
        int y = CONTENT_Y + 2 + i * 13;
        if (l[0] == '#') draw_section(y, l + 1);
        else gfx_text(10, y, l, FONT, C_TEXT, TRANSPARENT);
    }
    draw_scrollbar(LCD_W - 3, CONTENT_Y, CONTENT_H, n, scroll, visible);
}

static bool help_key(const key_event_t *ev) {
    if (ev->key == KEY_UP || ev->key == KEY_PAGE_UP) scroll -= ev->key == KEY_UP ? 1 : 10;
    else if (ev->key == KEY_DOWN || ev->key == KEY_PAGE_DOWN) scroll += ev->key == KEY_DOWN ? 1 : 10;
    else return false;
    return true;
}

const screen_t scr_help = {
    .tab_label = "Help",
    .title = "Help",
    .draw = draw_help,
    .key = help_key,
};

static void draw_logo(int cx, int cy) {
    // Simplified Meshtastic "M" mark: two slanted strokes
    for (int t = -2; t <= 2; t++) {
        gfx_line(cx - 30 + t, cy + 18, cx - 8 + t, cy - 18, C_ACCENT);
        gfx_line(cx - 8 + t, cy - 18, cx + 14 + t, cy + 18, C_ACCENT);
        gfx_line(cx + 4 + t, cy + 18, cx + 18 + t, cy - 4, C_ACCENT);
        gfx_line(cx + 18 + t, cy - 4, cx + 32 + t, cy + 18, C_ACCENT);
    }
}

static void draw_about(void) {
    draw_logo(LCD_W / 2, CONTENT_Y + 40);
    int y = CONTENT_Y + 70;
    gfx_text_center(LCD_W / 2, y, "Meshtastic for PicoCalc", FONT_BIG, C_TEXT, TRANSPARENT);
    y += 20;
    gfx_text_center(LCD_W / 2, y, "Version " APP_VERSION, FONT, C_DIM, TRANSPARENT);
    y += 22;
    static const char *const lines[] = {
        "A Bluetooth LE client for Meshtastic radios",
        "(Heltec V3 / V4, T-Beam, RAK, ...).",
        "",
        "Meshtastic(R) is a registered trademark of",
        "Meshtastic LLC. This is an unofficial client.",
        "",
        "Built with: Raspberry Pi pico-sdk, BTstack,",
        "nanopb, FatFs, Spleen font, Meshtastic",
        "protobufs (GPL-3.0).",
    };
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        gfx_text_center(LCD_W / 2, y, lines[i], FONT, i >= 6 ? C_FAINT : C_TEXT, TRANSPARENT);
        y += 12;
    }
}

const screen_t scr_about = {
    .tab_label = "Info",
    .title = "About",
    .draw = draw_about,
};
