// F4 - Channels, LoRa summary and the share URL.
#include <stdio.h>
#include <string.h>

#include "ui/ui.h"
#include "mesh/mesh.h"

static int idx_map[MAX_CHANNELS];
static int count;
static listnav_t nav;
static bool show_url;
static int url_scroll;
static char url[1024];

static void rebuild(void) {
    count = 0;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (mesh_channel_active(i)) idx_map[count++] = i;
    }
    nav.count = count;
}

static void enter(void) { rebuild(); }

static const char *psk_desc(const meshtastic_ChannelSettings *s) {
    switch (s->psk.size) {
    case 0: return "No encryption";
    case 1: return s->psk.bytes[0] == 0 ? "No encryption" : s->psk.bytes[0] == 1 ? "Default key" : "Simple key";
    case 16: return "AES-128";
    case 32: return "AES-256";
    default: return "Custom key";
    }
}

static void draw(void) {
    rebuild();
    int y = CONTENT_Y + 2;
    if (!g_mesh.config_complete && !count) {
        gfx_text_center(LCD_W / 2, 120, "Connect to a radio to see channels", FONT, C_DIM, TRANSPARENT);
        return;
    }
    if (show_url) {
        draw_section(y, "Share URL (QR codes need the phone app)");
        y += 16;
        if (!mesh_channel_url(url, sizeof(url))) snprintf(url, sizeof(url), "(unavailable)");
        wrap_line_t lines[24];
        int n = wrap_text(url, 52, lines, 24);
        if (url_scroll > n - 1) url_scroll = n - 1;
        if (url_scroll < 0) url_scroll = 0;
        for (int i = url_scroll; i < n && y < CONTENT_BOTTOM - 26; i++) {
            gfx_text_b(4, y, url + lines[i].start, lines[i].len, FONT, C_TEXT, TRANSPARENT);
            y += 12;
        }
        gfx_text(4, CONTENT_BOTTOM - 14, "The URL is also printed to the USB serial log.", FONT, C_FAINT, TRANSPARENT);
        return;
    }

    draw_section(y, "Channels");
    y += 14;
    list_clamp(&nav, MAX_CHANNELS);
    for (int i = 0; i < count; i++) {
        int ci = idx_map[i];
        const meshtastic_Channel *c = &g_mesh.channels[ci];
        bool sel = i == nav.sel;
        if (sel) gfx_fill(0, y, LCD_W, 26, C_SURFACE);
        gfx_round_fill(6, y + 3, 22, 20, 4, ci == 0 ? C_ACCENT : C_ACCENT_DARK);
        char b[4];
        snprintf(b, sizeof(b), "%d", ci);
        gfx_text_center(17, y + 7, b, FONT, ci == 0 ? C_ON_ACCENT : C_TEXT, TRANSPARENT);
        char name[24];
        mesh_channel_name(ci, name, sizeof(name));
        gfx_text(34, y + 1, name, FONT, C_TEXT, TRANSPARENT);
        int unread = mesh_conv_unread(CONV_CHANNEL(ci));
        if (unread) gfx_printf(LCD_W - 60, y + 1, FONT, C_ACCENT, TRANSPARENT, "%d new", unread);
        const meshtastic_ChannelSettings *s = &c->settings;
        gfx_printf(34, y + 13, FONT, C_DIM, TRANSPARENT, "%s  %s%s%s", c->role == meshtastic_Channel_Role_PRIMARY ? "Primary" : "Secondary",
                   psk_desc(s), s->uplink_enabled ? "  Up" : "", s->downlink_enabled ? "  Down" : "");
        y += 27;
    }

    y += 4;
    draw_section(y, "LoRa");
    y += 14;
    if (g_mesh.has_lora) {
        const meshtastic_Config_LoRaConfig *l = &g_mesh.lora;
        gfx_printf(8, y, FONT, C_TEXT, TRANSPARENT, "Region  %s", mesh_region_name(l->region));
        gfx_printf(160, y, FONT, C_TEXT, TRANSPARENT, "Preset  %s", l->use_preset ? mesh_preset_name(l->modem_preset) : "Custom");
        y += 12;
        gfx_printf(8, y, FONT, C_TEXT, TRANSPARENT, "Hops    %lu", (unsigned long)l->hop_limit);
        gfx_printf(160, y, FONT, C_TEXT, TRANSPARENT, "TX      %s %ddBm", l->tx_enabled ? "on" : "OFF", l->tx_power);
        y += 12;
        gfx_printf(8, y, FONT, C_TEXT, TRANSPARENT, "Slot    %u", l->channel_num);
        if (l->override_frequency > 0) gfx_printf(160, y, FONT, C_TEXT, TRANSPARENT, "Freq    %.3f", l->override_frequency);
        y += 12;
    } else {
        gfx_text(8, y, "Unknown", FONT, C_DIM, TRANSPARENT);
        y += 12;
    }
    gfx_text(4, CONTENT_BOTTOM - 14, "Enter: open chat   U: share URL", FONT, C_FAINT, TRANSPARENT);
}

static bool key(const key_event_t *ev) {
    if (show_url) {
        switch (ev->key) {
        case KEY_UP: url_scroll--; return true;
        case KEY_DOWN: url_scroll++; return true;
        case KEY_ESC: case 'u': case 'U': show_url = false; return true;
        }
        return false;
    }
    rebuild();
    if (list_key(&nav, ev, MAX_CHANNELS)) return true;
    if (ev->key == KEY_ENTER && count) {
        messages_open_conversation(CONV_CHANNEL(idx_map[nav.sel]));
        return true;
    }
    if (ev->key == 'u' || ev->key == 'U') {
        show_url = true;
        url_scroll = 0;
        if (mesh_channel_url(url, sizeof(url))) printf("Channel URL: %s\n", url);
        return true;
    }
    return false;
}

const screen_t scr_channels = {
    .tab_label = "Chan",
    .title = "Channels",
    .enter = enter,
    .draw = draw,
    .key = key,
};
