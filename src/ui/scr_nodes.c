// F2 - Node list with filter and node detail / actions.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ui/ui.h"
#include "mesh/mesh.h"
#include "storage.h"

#define ROW_H 34
#define FILTER_H 18

static int order[MAX_NODES];
static int order_count;
static listnav_t nav;
static textbox_t filter;
static bool in_detail;
static uint32_t detail_num;
static listnav_t act_nav;
static char title_buf[48];

static uint32_t heard_key(const node_t *n) {
    // Prefer radio timestamps; fall back to local uptime ordering.
    if (n->last_heard) return n->last_heard;
    return n->last_heard_up;
}

static int cmp_nodes(const void *a, const void *b) {
    const node_t *x = &g_mesh.nodes[*(const int *)a];
    const node_t *y = &g_mesh.nodes[*(const int *)b];
    if (x->num == g_mesh.my_num) return -1;
    if (y->num == g_mesh.my_num) return 1;
    if (x->is_favorite != y->is_favorite) return x->is_favorite ? -1 : 1;
    uint32_t hx = heard_key(x), hy = heard_key(y);
    if (hx != hy) return hx > hy ? -1 : 1;
    return x->num < y->num ? -1 : 1;
}

static bool contains_ci(const char *hay, const char *needle) {
    if (!*needle) return true;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n) return true;
    }
    return false;
}

static void rebuild(void) {
    order_count = 0;
    for (int i = 0; i < g_mesh.node_count; i++) {
        const node_t *n = &g_mesh.nodes[i];
        if (filter.len && !contains_ci(n->long_name, filter.buf) && !contains_ci(n->short_name, filter.buf) &&
            !contains_ci(n->id, filter.buf))
            continue;
        order[order_count++] = i;
    }
    qsort(order, (size_t)order_count, sizeof(int), cmp_nodes);
    nav.count = order_count;
}

void nodes_open_detail(uint32_t num) {
    in_detail = true;
    detail_num = num;
    act_nav.sel = act_nav.top = 0;
    ui_goto_tab(TAB_NODES);
}

static void enter(void) {
    if (filter.max_bytes == 0) tb_init(&filter, 24, NULL);
}

static const char *dyn_title(void) {
    if (!in_detail) return NULL;
    mesh_node_longname(detail_num, title_buf, sizeof(title_buf));
    return title_buf;
}

static void hops_text(const node_t *n, char *out, size_t sz) {
    if (n->num == g_mesh.my_num) snprintf(out, sz, "This radio");
    else if (n->via_mqtt) snprintf(out, sz, "MQTT");
    else if (n->hops_away == 0) snprintf(out, sz, "Direct %.1fdB", n->snr);
    else if (n->hops_away > 0) snprintf(out, sz, "%d hop%s", n->hops_away, n->hops_away == 1 ? "" : "s");
    else snprintf(out, sz, "? hops");
}

// ------------------------------------------------------------------ list

static int list_visible(void) { return (CONTENT_H - (filter.len ? FILTER_H : 0)) / ROW_H; }

static void draw_list(void) {
    rebuild();
    int top_y = CONTENT_Y;
    if (filter.len) {
        gfx_fill(0, top_y, LCD_W, FILTER_H, C_BAR);
        gfx_text(4, top_y + 3, "Filter:", FONT, C_DIM, TRANSPARENT);
        tb_draw(&filter, 52, top_y + 1, LCD_W - 56, FONT, C_TEXT, C_SURFACE, true);
        top_y += FILTER_H;
    }
    int visible = list_visible();
    list_clamp(&nav, visible);
    if (!order_count) {
        gfx_text_center(LCD_W / 2, 130, filter.len ? "No matching nodes" : "No nodes yet", FONT, C_DIM, TRANSPARENT);
        if (!filter.len) gfx_text_center(LCD_W / 2, 146, "Type to filter once nodes appear", FONT, C_FAINT, TRANSPARENT);
        return;
    }
    for (int i = 0; i < visible && nav.top + i < order_count; i++) {
        int idx = nav.top + i;
        const node_t *n = &g_mesh.nodes[order[idx]];
        int y = top_y + i * ROW_H;
        if (idx == nav.sel) gfx_fill(0, y, LCD_W, ROW_H, C_SURFACE);
        gfx_hline(52, y + ROW_H - 1, LCD_W - 52, C_SURFACE);
        draw_node_badge(6, y + 5, 40, 24, n->num, FONT);
        if (mesh_node_is_online(n) || n->num == g_mesh.my_num) gfx_fill_circle(44, y + 7, 3, C_ACCENT);

        char ago[12];
        mesh_format_ago(n->last_heard, n->last_heard_up, ago, sizeof(ago));
        if (n->num == g_mesh.my_num) snprintf(ago, sizeof(ago), "now");
        gfx_text_right(LCD_W - 6, y + 3, ago, FONT, C_DIM, TRANSPARENT);
        int nx = 52;
        if (n->is_favorite) nx = gfx_text(nx, y + 3, "* ", FONT, C_YELLOW, TRANSPARENT);
        char name[40];
        mesh_node_longname(n->num, name, sizeof(name));
        gfx_text_fit(nx, y + 3, name, LCD_W - 12 - gfx_text_width(ago, FONT) - nx, FONT, C_TEXT, TRANSPARENT);

        int x = 52;
        char buf[24];
        hops_text(n, buf, sizeof(buf));
        x = gfx_text(x, y + 18, buf, FONT, C_DIM, TRANSPARENT) + 8;
        float d;
        if (mesh_node_distance(n, &d, NULL)) {
            ui_format_distance(d, buf, sizeof(buf));
            x = gfx_text(x, y + 18, buf, FONT, C_DIM, TRANSPARENT) + 8;
        }
        if (n->battery >= 0) {
            if (n->battery > 100) {
                gfx_text_right(LCD_W - 6, y + 18, "PWR", FONT, C_DIM, TRANSPARENT);
            } else {
                snprintf(buf, sizeof(buf), "%d%%", n->battery);
                gfx_text_right(LCD_W - 6, y + 18, buf, FONT, C_DIM, TRANSPARENT);
                draw_battery(LCD_W - 30 - gfx_text_width(buf, FONT), y + 19, n->battery, false);
            }
        }
    }
    draw_scrollbar(LCD_W - 3, top_y, visible * ROW_H, order_count, nav.top, visible);
}

// ------------------------------------------------------------------ detail

enum { ACT_DM, ACT_TRACEROUTE, ACT_POSITION, ACT_USERINFO, ACT_FAVORITE, ACT_MAP, ACT_REMOVE, ACT_COUNT };

static void action_label(int a, const node_t *n, char *out, size_t sz) {
    switch (a) {
    case ACT_DM: snprintf(out, sz, "Direct message"); break;
    case ACT_TRACEROUTE: snprintf(out, sz, "Traceroute"); break;
    case ACT_POSITION: snprintf(out, sz, "Request position"); break;
    case ACT_USERINFO: snprintf(out, sz, "Exchange user info"); break;
    case ACT_FAVORITE: snprintf(out, sz, n && n->is_favorite ? "Remove from favourites" : "Add to favourites"); break;
    case ACT_MAP: snprintf(out, sz, "Show on map"); break;
    case ACT_REMOVE: snprintf(out, sz, "Remove node"); break;
    }
}

static void do_remove(void) {
    if (mesh_admin_remove_node(detail_num)) {
        in_detail = false;
        ui_toast("Node removed");
    } else {
        ui_toast("Not connected");
    }
}

static void run_action(int a) {
    node_t *n = mesh_node_find(detail_num);
    bool ok = true;
    bool self = detail_num == g_mesh.my_num;
    switch (a) {
    case ACT_DM:
        if (self) {
            ui_toast("That's this radio");
            return;
        }
        messages_open_conversation(detail_num);
        return;
    case ACT_TRACEROUTE:
        ok = !self && mesh_send_traceroute(detail_num);
        if (ok) ui_toast("Traceroute sent");
        break;
    case ACT_POSITION:
        ok = !self && mesh_request_position(detail_num);
        if (ok) ui_toast("Position requested");
        break;
    case ACT_USERINFO:
        ok = !self && mesh_exchange_user_info(detail_num);
        if (ok) ui_toast("User info sent");
        break;
    case ACT_FAVORITE:
        ok = n && mesh_admin_set_favorite(detail_num, !n->is_favorite);
        break;
    case ACT_MAP:
        if (!n || !n->has_pos) {
            ui_toast("Position unknown");
            return;
        }
        map_focus_node(detail_num);
        return;
    case ACT_REMOVE:
        if (self) return;
        ui_confirm("Remove node", "Remove this node from the radio's node database?", do_remove);
        return;
    }
    if (!ok) ui_toast(self ? "Not available for this radio" : "Not connected");
}

static int kv(int y, const char *k, const char *v, uint16_t vc) {
    gfx_text(8, y, k, FONT, C_DIM, TRANSPARENT);
    gfx_text_fit(96, y, v, LCD_W - 100, FONT, vc, TRANSPARENT);
    return y + 12;
}

static void draw_detail(void) {
    const node_t *n = mesh_node_find(detail_num);
    if (!n) {
        gfx_text_center(LCD_W / 2, 120, "Node no longer known", FONT, C_DIM, TRANSPARENT);
        return;
    }
    int y = CONTENT_Y + 4;
    draw_node_badge(8, y, 52, 30, n->num, FONT_BIG);
    char name[40];
    mesh_node_longname(n->num, name, sizeof(name));
    gfx_text_fit(68, y, name, LCD_W - 72, FONT_BIG, C_TEXT, TRANSPARENT);
    gfx_printf(68, y + 18, FONT, C_DIM, TRANSPARENT, "%s  %s%s", n->id, mesh_hw_name(n->hw_model),
               n->is_favorite ? "  *" : "");
    y += 36;

    char buf[64], b2[24];
    snprintf(buf, sizeof(buf), "%s", mesh_role_name(n->role));
    y = kv(y, "Role", buf, C_TEXT);
    mesh_format_ago(n->last_heard, n->last_heard_up, b2, sizeof(b2));
    if (n->last_heard) {
        char t[20];
        ui_format_datetime(n->last_heard, t, sizeof(t));
        snprintf(buf, sizeof(buf), "%s (%s)", b2, t);
    } else {
        snprintf(buf, sizeof(buf), "%s", b2);
    }
    y = kv(y, "Last heard", n->num == g_mesh.my_num ? "now" : buf, C_TEXT);
    hops_text(n, buf, sizeof(buf));
    y = kv(y, "Route", buf, C_TEXT);
    if (n->battery >= 0 || n->voltage > 0) {
        if (n->battery > 100) snprintf(buf, sizeof(buf), "Powered  %.2fV", n->voltage);
        else snprintf(buf, sizeof(buf), "%d%%  %.2fV", n->battery, n->voltage);
        y = kv(y, "Battery", buf, C_TEXT);
    }
    if (n->has_metrics) {
        snprintf(buf, sizeof(buf), "ChUtil %.1f%%  AirTx %.1f%%", n->ch_util, n->air_util);
        y = kv(y, "Airtime", buf, C_TEXT);
    }
    if (n->has_pos) {
        snprintf(buf, sizeof(buf), "%.5f, %.5f  %ldm", n->lat_i * 1e-7, n->lon_i * 1e-7, (long)n->alt);
        y = kv(y, "Position", buf, C_TEXT);
        float d, brg;
        if (mesh_node_distance(n, &d, &brg)) {
            ui_format_distance(d, b2, sizeof(b2));
            snprintf(buf, sizeof(buf), "%s at %.0f deg", b2, brg);
            y = kv(y, "Distance", buf, C_TEXT);
        }
    }
    if (n->has_env) {
        snprintf(buf, sizeof(buf), "%.1fC  %.0f%%RH  %.0fhPa", n->temperature, n->humidity, n->pressure);
        y = kv(y, "Environment", buf, C_TEXT);
    }
    y = kv(y, "Encryption", n->has_pubkey ? "PKI (public key known)" : "Channel key", n->has_pubkey ? C_ACCENT : C_DIM);

    y += 4;
    draw_section(y, "Actions");
    y += 14;
    act_nav.count = ACT_COUNT;
    for (int a = 0; a < ACT_COUNT; a++) {
        bool sel = a == act_nav.sel;
        if (sel) gfx_fill(4, y, LCD_W - 8, 13, C_ACCENT);
        action_label(a, n, buf, sizeof(buf));
        gfx_text(10, y + 1, buf, FONT, sel ? C_ON_ACCENT : C_TEXT, TRANSPARENT);
        y += 13;
    }
    if (g_mesh.tr_target == n->num && g_mesh.tr_result[0]) {
        y += 4;
        wrap_line_t lines[8];
        int nl = wrap_text(g_mesh.tr_result, 50, lines, 8);
        for (int i = 0; i < nl && y < CONTENT_BOTTOM - 12; i++) {
            gfx_text_b(8, y, g_mesh.tr_result + lines[i].start, lines[i].len, FONT,
                       g_mesh.tr_pending ? C_DIM : C_YELLOW, TRANSPARENT);
            y += 12;
        }
    }
}

static bool key(const key_event_t *ev) {
    if (in_detail) {
        if (ev->key == KEY_ESC || ev->key == KEY_BACKSPACE) {
            in_detail = false;
            return true;
        }
        act_nav.count = ACT_COUNT;
        if (list_key(&act_nav, ev, ACT_COUNT)) return true;
        if (ev->key == KEY_ENTER) {
            run_action(act_nav.sel);
            return true;
        }
        return false;
    }
    rebuild();
    if (list_key(&nav, ev, list_visible())) return true;
    if (ev->key == KEY_ENTER) {
        if (order_count) {
            in_detail = true;
            detail_num = g_mesh.nodes[order[nav.sel]].num;
            act_nav.sel = act_nav.top = 0;
        }
        return true;
    }
    if (ev->key == KEY_ESC) {
        if (filter.len) {
            tb_clear(&filter);
            nav.sel = nav.top = 0;
            return true;
        }
        return false;
    }
    int r = tb_key(&filter, ev);
    if (r == TB_CHANGED) {
        nav.sel = nav.top = 0;
        return true;
    }
    return r == TB_NONE;
}

static void draw(void) {
    if (in_detail) draw_detail();
    else draw_list();
}

const screen_t scr_nodes = {
    .tab_label = "Node",
    .title = "Nodes",
    .enter = enter,
    .draw = draw,
    .key = key,
    .dyn_title = dyn_title,
};
