// F7 - Statistics for the connected radio, the mesh and this PicoCalc.
#include <stdio.h>
#include <string.h>

#include "ui/ui.h"
#include "mesh/mesh.h"
#include "mesh/ble_link.h"

static int kv(int x, int y, const char *k, const char *v) {
    gfx_text(x, y, k, FONT, C_DIM, TRANSPARENT);
    gfx_text_fit(x + 84, y, v, LCD_W - x - 88, FONT, C_TEXT, TRANSPARENT);
    return y + 12;
}

static int bar(int y, const char *label, float pct, uint16_t col) {
    gfx_text(8, y, label, FONT, C_DIM, TRANSPARENT);
    int x = 92, w = LCD_W - 150;
    gfx_fill(x, y + 2, w, 8, C_SURFACE2);
    if (pct > 100) pct = 100;
    if (pct > 0) gfx_fill(x, y + 2, (int)(w * pct / 100), 8, col);
    gfx_printf(x + w + 6, y, FONT, C_TEXT, TRANSPARENT, "%.1f%%", pct);
    return y + 13;
}

static void fmt_uptime(uint32_t s, char *out, size_t n) {
    if (s >= 86400) snprintf(out, n, "%lud %luh", (unsigned long)(s / 86400), (unsigned long)(s % 86400 / 3600));
    else snprintf(out, n, "%luh %lum", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
}

static void draw(void) {
    int y = CONTENT_Y + 4;
    char buf[64];
    const node_t *me = mesh_my_node();

    draw_section(y, "Connected radio");
    y += 14;
    if (!me) {
        gfx_text(8, y, ble_link_state() == LINK_CONNECTED ? "Loading..." : "Not connected", FONT, C_DIM, TRANSPARENT);
        y += 14;
    } else {
        y = kv(8, y, "Name", me->long_name);
        snprintf(buf, sizeof(buf), "%s (%s)", me->id, me->short_name);
        y = kv(8, y, "ID", buf);
        y = kv(8, y, "Hardware", mesh_hw_name(me->hw_model));
        y = kv(8, y, "Firmware", g_mesh.has_metadata ? g_mesh.metadata.firmware_version : "?");
        y = kv(8, y, "Role", mesh_role_name(g_mesh.has_device ? g_mesh.device.role : me->role));
        if (me->battery >= 0) {
            if (me->battery > 100) snprintf(buf, sizeof(buf), "External power  %.2fV", me->voltage);
            else snprintf(buf, sizeof(buf), "%d%%  %.2fV", me->battery, me->voltage);
            y = kv(8, y, "Battery", buf);
        }
        if (me->uptime) {
            fmt_uptime(me->uptime, buf, sizeof(buf));
            y = kv(8, y, "Uptime", buf);
        }
        y += 2;
        y = bar(y, "Channel util", me->ch_util, me->ch_util > 25 ? C_YELLOW : C_ACCENT);
        y = bar(y, "Air util TX", me->air_util, me->air_util > 10 ? C_YELLOW : C_ACCENT);
    }

    y += 4;
    draw_section(y, "Mesh");
    y += 14;
    int total = g_mesh.node_count > 0 && me ? g_mesh.node_count - 1 : g_mesh.node_count;
    snprintf(buf, sizeof(buf), "%d online / %d known", mesh_nodes_online(), total);
    y = kv(8, y, "Nodes", buf);
    snprintf(buf, sizeof(buf), "%lu received, %lu sent", (unsigned long)g_mesh.rx_packets, (unsigned long)g_mesh.tx_packets);
    y = kv(8, y, "Packets", buf);
    snprintf(buf, sizeof(buf), "%lu received", (unsigned long)g_mesh.rx_text);
    y = kv(8, y, "Messages", buf);
    if (g_mesh.queue_max) {
        snprintf(buf, sizeof(buf), "%u of %u free", g_mesh.queue_free, g_mesh.queue_max);
        y = kv(8, y, "TX queue", buf);
    }

    y += 4;
    draw_section(y, "PicoCalc");
    y += 14;
    bool chg;
    int pct = kbd_battery_percent(&chg);
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%%s", pct, chg ? " (charging)" : "");
    else snprintf(buf, sizeof(buf), "not reported");
    y = kv(8, y, "Battery", buf);
    fmt_uptime(mesh_uptime_s(), buf, sizeof(buf));
    y = kv(8, y, "Uptime", buf);
    if (ble_link_state() == LINK_CONNECTED)
        snprintf(buf, sizeof(buf), "%s  MTU %u", ble_link_state_name(), ble_link_mtu());
    else
        snprintf(buf, sizeof(buf), "%s", ble_link_state_name());
    y = kv(8, y, "Bluetooth", buf);
    (void)y;
}

const screen_t scr_stats = {
    .tab_label = "Stat",
    .title = "Statistics",
    .draw = draw,
};
