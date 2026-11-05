// F3 - Offline node map: relative plot (no tiles) with range rings.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui.h"
#include "mesh/mesh.h"
#include "storage.h"

static const float scales[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000}; // metres/pixel
#define NSCALES ((int)(sizeof(scales) / sizeof(scales[0])))

static bool initialised;
static double clat, clon; // map centre (degrees)
static int zoom = 6;
static uint32_t selected;

static const node_t *first_positioned(void) {
    const node_t *me = mesh_my_node();
    if (me && me->has_pos) return me;
    for (int i = 0; i < g_mesh.node_count; i++) {
        if (g_mesh.nodes[i].has_pos) return &g_mesh.nodes[i];
    }
    return NULL;
}

static void fit_all(void) {
    const node_t *c = first_positioned();
    if (!c) return;
    clat = c->lat_i * 1e-7;
    clon = c->lon_i * 1e-7;
    double maxd = 0;
    double coslat = cos(clat * M_PI / 180);
    for (int i = 0; i < g_mesh.node_count; i++) {
        const node_t *n = &g_mesh.nodes[i];
        if (!n->has_pos) continue;
        double dx = fabs((n->lon_i * 1e-7 - clon) * coslat * 111320.0);
        double dy = fabs((n->lat_i * 1e-7 - clat) * 110540.0);
        if (dx > maxd) maxd = dx;
        if (dy > maxd) maxd = dy;
    }
    zoom = NSCALES - 1;
    for (int z = 0; z < NSCALES; z++) {
        if (maxd / scales[z] < 130) {
            zoom = z;
            break;
        }
    }
    initialised = true;
}

void map_focus_node(uint32_t num) {
    const node_t *n = mesh_node_find(num);
    if (n && n->has_pos) {
        clat = n->lat_i * 1e-7;
        clon = n->lon_i * 1e-7;
        initialised = true;
        selected = num;
    }
    ui_goto_tab(TAB_MAP);
}

static void enter(void) {
    if (!initialised) fit_all();
}

static void project(int32_t lat_i, int32_t lon_i, int *px, int *py) {
    double coslat = cos(clat * M_PI / 180);
    double dx = (lon_i * 1e-7 - clon) * coslat * 111320.0;
    double dy = (lat_i * 1e-7 - clat) * 110540.0;
    *px = LCD_W / 2 + (int)lround(dx / scales[zoom]);
    *py = CONTENT_Y + CONTENT_H / 2 - (int)lround(dy / scales[zoom]);
}

static float nice_distance(float m) {
    static const float steps[] = {1, 2, 5};
    float base = 1;
    while (base * 10 <= m) base *= 10;
    float best = base;
    for (int i = 0; i < 3; i++) {
        if (steps[i] * base <= m) best = steps[i] * base;
    }
    return best;
}

static void draw(void) {
    if (!initialised) fit_all();
    if (!initialised) {
        gfx_text_center(LCD_W / 2, 120, "No node positions yet", FONT, C_DIM, TRANSPARENT);
        gfx_text_center(LCD_W / 2, 136, "Nodes appear here once they share GPS", FONT, C_FAINT, TRANSPARENT);
        return;
    }
    // Range rings around our own node (or the centre)
    const node_t *me = mesh_my_node();
    int ox, oy;
    if (me && me->has_pos) project(me->lat_i, me->lon_i, &ox, &oy);
    else { ox = LCD_W / 2; oy = CONTENT_Y + CONTENT_H / 2; }
    float ring = nice_distance(scales[zoom] * 60);
    char lbl[20];
    for (int i = 1; i <= 4; i++) {
        int r = (int)(ring * i / scales[zoom]);
        gfx_circle(ox, oy, r, C_SURFACE2);
        ui_format_distance(ring * i, lbl, sizeof(lbl));
        gfx_text(ox + 2, oy - r + 1, lbl, FONT, C_FAINT, TRANSPARENT);
    }
    gfx_hline(ox - 4, oy, 9, C_FAINT);
    gfx_vline(ox, oy - 4, 9, C_FAINT);

    // Nodes (selected drawn last so it is on top)
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_mesh.node_count; i++) {
            const node_t *n = &g_mesh.nodes[i];
            if (!n->has_pos) continue;
            bool sel = n->num == selected;
            if ((pass == 1) != sel) continue;
            int x, y;
            project(n->lat_i, n->lon_i, &x, &y);
            if (x < -20 || x > LCD_W + 20 || y < CONTENT_Y - 20 || y > CONTENT_BOTTOM + 20) continue;
            uint16_t c = RGB888_TO_565(mesh_node_color(n->num));
            bool online = mesh_node_is_online(n) || n->num == g_mesh.my_num;
            if (sel) gfx_fill_circle(x, y, 8, C_WHITE);
            gfx_fill_circle(x, y, 6, online ? c : C_FAINT);
            if (n->num == g_mesh.my_num) gfx_circle(x, y, 8, C_ACCENT);
            char sn[8];
            mesh_node_label(n->num, sn, sizeof(sn));
            gfx_text(x + 9, y - 6, sn, FONT, sel ? C_WHITE : (online ? C_TEXT : C_DIM), sel ? (int32_t)C_BG : TRANSPARENT);
        }
    }

    // North indicator and scale
    gfx_text(LCD_W - 14, CONTENT_Y + 4, "N", FONT_BIG, C_TEXT, TRANSPARENT);
    gfx_line(LCD_W - 10, CONTENT_Y + 22, LCD_W - 10, CONTENT_Y + 32, C_TEXT);
    float sb = nice_distance(scales[zoom] * 80);
    int sbw = (int)(sb / scales[zoom]);
    int sy = CONTENT_BOTTOM - 10;
    gfx_hline(8, sy, sbw, C_TEXT);
    gfx_vline(8, sy - 3, 4, C_TEXT);
    gfx_vline(8 + sbw, sy - 3, 4, C_TEXT);
    ui_format_distance(sb, lbl, sizeof(lbl));
    gfx_text(12 + sbw, sy - 8, lbl, FONT, C_TEXT, TRANSPARENT);

    // Selection info panel
    if (selected) {
        const node_t *n = mesh_node_find(selected);
        if (n) {
            char name[40], ago[12], info[64];
            mesh_node_longname(n->num, name, sizeof(name));
            mesh_format_ago(n->last_heard, n->last_heard_up, ago, sizeof(ago));
            float d;
            if (mesh_node_distance(n, &d, NULL)) {
                char ds[16];
                ui_format_distance(d, ds, sizeof(ds));
                snprintf(info, sizeof(info), "%s  heard %s", ds, ago);
            } else {
                snprintf(info, sizeof(info), "heard %s", ago);
            }
            gfx_fill(0, CONTENT_Y, LCD_W - 24, 28, C_BAR);
            gfx_text_fit(6, CONTENT_Y + 2, name, LCD_W - 36, FONT, C_TEXT, TRANSPARENT);
            gfx_text_fit(6, CONTENT_Y + 14, info, LCD_W - 36, FONT, C_DIM, TRANSPARENT);
        }
    } else {
        gfx_text(6, CONTENT_Y + 4, "Arrows pan  +/- zoom  Tab select", FONT, C_FAINT, TRANSPARENT);
    }
}

static void select_next(int dir) {
    // Cycle through positioned nodes in node table order
    int count = g_mesh.node_count;
    if (!count) return;
    int start = -1;
    for (int i = 0; i < count; i++) {
        if (g_mesh.nodes[i].num == selected) start = i;
    }
    for (int k = 1; k <= count; k++) {
        int i = ((start < 0 ? (dir > 0 ? -1 : 0) : start) + dir * k + count * 2) % count;
        if (g_mesh.nodes[i].has_pos) {
            selected = g_mesh.nodes[i].num;
            clat = g_mesh.nodes[i].lat_i * 1e-7;
            clon = g_mesh.nodes[i].lon_i * 1e-7;
            return;
        }
    }
}

static bool key(const key_event_t *ev) {
    double step = 40.0 * scales[zoom];
    double coslat = cos(clat * M_PI / 180);
    if (coslat < 0.01) coslat = 0.01;
    switch (ev->key) {
    case KEY_UP: clat += step / 110540.0; break;
    case KEY_DOWN: clat -= step / 110540.0; break;
    case KEY_LEFT: clon -= step / (111320.0 * coslat); break;
    case KEY_RIGHT: clon += step / (111320.0 * coslat); break;
    case '+': case '=': if (zoom > 0) zoom--; break;
    case '-': case '_': if (zoom < NSCALES - 1) zoom++; break;
    case 'c': case 'C': {
        const node_t *me = mesh_my_node();
        if (me && me->has_pos) {
            clat = me->lat_i * 1e-7;
            clon = me->lon_i * 1e-7;
        }
        break;
    }
    case 'f': case 'F': fit_all(); break;
    case KEY_TAB: select_next((ev->mods & MOD_SHIFT) ? -1 : 1); break;
    case KEY_ENTER:
        if (selected) nodes_open_detail(selected);
        break;
    case KEY_ESC:
        if (!selected) return false;
        selected = 0;
        break;
    default: return false;
    }
    return true;
}

const screen_t scr_map = {
    .tab_label = "Map",
    .title = "Map",
    .enter = enter,
    .draw = draw,
    .key = key,
};
