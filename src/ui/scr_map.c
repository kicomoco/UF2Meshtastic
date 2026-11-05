// F3 - Node map: Web Mercator with range rings, drawn over downloaded map
// tiles when a tile source is enabled (see map/tiles.c).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui.h"
#include "map/tiles.h"
#include "mesh/mesh.h"
#include "storage.h"

#define MIN_ZOOM 2
#define MAX_ZOOM 19
#define VIEW_CX (LCD_W / 2)
#define VIEW_CY (CONTENT_H / 2)

static bool initialised;
static double clat, clon; // map centre (degrees)
static int zoom = 3;
static uint32_t selected;

static double world_size(int z) { return 256.0 * (double)(1u << z); }

static void to_world(double lat, double lon, int z, double *wx, double *wy) {
    if (lat > 85.0511) lat = 85.0511;
    if (lat < -85.0511) lat = -85.0511;
    double r = lat * M_PI / 180;
    *wx = (lon + 180) / 360 * world_size(z);
    *wy = (1 - asinh(tan(r)) / M_PI) / 2 * world_size(z);
}

static void from_world(double wx, double wy, int z, double *lat, double *lon) {
    double n = world_size(z);
    *lon = wx / n * 360 - 180;
    while (*lon < -180) *lon += 360;
    while (*lon >= 180) *lon -= 360;
    *lat = atan(sinh(M_PI * (1 - 2 * wy / n))) * 180 / M_PI;
}

static double metres_per_px(void) { return 156543.03392 * cos(clat * M_PI / 180) / (double)(1u << zoom); }

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
    for (zoom = 16; zoom > MIN_ZOOM; zoom--) {
        if (maxd / metres_per_px() < 130) break;
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

static double cwx, cwy; // centre in world pixels for the current frame

static void project(int32_t lat_i, int32_t lon_i, int *px, int *py) {
    double wx, wy;
    to_world(lat_i * 1e-7, lon_i * 1e-7, zoom, &wx, &wy);
    double dx = wx - cwx, n = world_size(zoom);
    if (dx > n / 2) dx -= n;
    if (dx < -n / 2) dx += n;
    *px = VIEW_CX + (int)lround(dx);
    *py = CONTENT_Y + VIEW_CY + (int)lround(wy - cwy);
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

// Text with a dark box behind it, readable over any map.
static int label(int x, int y, const char *s, uint16_t fg, bool boxed) {
    if (boxed) gfx_fill(x - 2, y - 1, gfx_text_width(s, FONT) + 4, 14, C_BG);
    return gfx_text(x, y, s, FONT, fg, TRANSPARENT);
}

static void draw(void) {
    if (!initialised) fit_all();
    to_world(clat, clon, zoom, &cwx, &cwy);
    tiles_set_view(zoom, (int32_t)lround(cwx) - VIEW_CX, (int32_t)lround(cwy) - VIEW_CY);
    const tile_source_t *src = tiles_active_source();
    bool tiles = tiles_draw(CONTENT_Y);
    bool light = tiles && !src->dark;
    bool boxed = tiles;

    if (!initialised) {
        if (!tiles) {
            gfx_text_center(LCD_W / 2, 120, "No node positions yet", FONT, C_DIM, TRANSPARENT);
            gfx_text_center(LCD_W / 2, 136, "Nodes appear here once they share GPS", FONT, C_FAINT, TRANSPARENT);
            return;
        }
    } else {
        // Range rings around our own node (or the centre)
        const node_t *me = mesh_my_node();
        int ox, oy;
        if (me && me->has_pos) project(me->lat_i, me->lon_i, &ox, &oy);
        else { ox = VIEW_CX; oy = CONTENT_Y + VIEW_CY; }
        double mpp = metres_per_px();
        float ring = nice_distance((float)(mpp * 60));
        uint16_t ring_c = light ? RGB565(0x50, 0x58, 0x60) : (tiles ? C_DIM : C_SURFACE2);
        char lbl[20];
        for (int i = 1; i <= 4; i++) {
            int r = (int)(ring * i / mpp);
            gfx_circle(ox, oy, r, ring_c);
            ui_format_distance(ring * i, lbl, sizeof(lbl));
            label(ox + 3, oy - r + 2, lbl, tiles ? C_DIM : C_FAINT, boxed);
        }
        gfx_hline(ox - 4, oy, 9, ring_c);
        gfx_vline(ox, oy - 4, 9, ring_c);

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
                else if (tiles) gfx_fill_circle(x, y, 7, C_BG);
                gfx_fill_circle(x, y, 6, online ? c : C_FAINT);
                if (n->num == g_mesh.my_num) gfx_circle(x, y, 8, C_ACCENT);
                char sn[8];
                mesh_node_label(n->num, sn, sizeof(sn));
                uint16_t fg = sel ? C_WHITE : (online ? C_TEXT : C_DIM);
                if (sel && !boxed) gfx_text(x + 9, y - 6, sn, FONT, fg, C_BG);
                else label(x + 10, y - 6, sn, fg, boxed);
            }
        }
    }

    // North indicator, scale and attribution
    if (boxed) gfx_fill(LCD_W - 18, CONTENT_Y + 2, 16, 34, C_BG);
    gfx_text(LCD_W - 14, CONTENT_Y + 4, "N", FONT_BIG, C_TEXT, TRANSPARENT);
    gfx_line(LCD_W - 10, CONTENT_Y + 22, LCD_W - 10, CONTENT_Y + 32, C_TEXT);
    int bottom = CONTENT_BOTTOM;
    if (tiles) {
        int w = gfx_text_width(src->attribution, FONT);
        gfx_fill(LCD_W - w - 4, bottom - 13, w + 4, 13, C_BG);
        gfx_text(LCD_W - w - 2, bottom - 12, src->attribution, FONT, C_DIM, TRANSPARENT);
        bottom -= 14;
    }
    if (initialised || tiles) {
        double mpp = metres_per_px();
        float sb = nice_distance((float)(mpp * 80));
        int sbw = (int)(sb / mpp);
        char lbl[20];
        ui_format_distance(sb, lbl, sizeof(lbl));
        int sy = bottom - 6;
        if (boxed) gfx_fill(4, sy - 10, sbw + 12 + gfx_text_width(lbl, FONT), 14, C_BG);
        gfx_hline(8, sy, sbw, C_TEXT);
        gfx_vline(8, sy - 3, 4, C_TEXT);
        gfx_vline(8 + sbw, sy - 3, 4, C_TEXT);
        gfx_text(12 + sbw, sy - 8, lbl, FONT, C_TEXT, TRANSPARENT);
    }

    // Selection info panel / hint, then tile status underneath
    int ty = CONTENT_Y + 4;
    const node_t *n = selected ? mesh_node_find(selected) : NULL;
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
        ty = CONTENT_Y + 32;
    } else {
        label(6, ty, "Arrows pan  +/- zoom  Tab select", tiles ? C_DIM : C_FAINT, boxed);
        ty += 16;
    }
    const char *st = tiles_status();
    if (st) label(6, ty, st, C_YELLOW, true);
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

static void pan(int dx, int dy) {
    double wx, wy;
    to_world(clat, clon, zoom, &wx, &wy);
    from_world(wx + dx, wy + dy, zoom, &clat, &clon);
    initialised = true;
}

static bool key(const key_event_t *ev) {
    switch (ev->key) {
    case KEY_UP: pan(0, -40); break;
    case KEY_DOWN: pan(0, 40); break;
    case KEY_LEFT: pan(-40, 0); break;
    case KEY_RIGHT: pan(40, 0); break;
    case '+': case '=': if (zoom < MAX_ZOOM) zoom++; break;
    case '-': case '_': if (zoom > MIN_ZOOM) zoom--; break;
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
