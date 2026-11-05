// Map tile background: decodes cached PNG tiles into an 8-bit indexed
// buffer (the full RGB565 view would need another 180 KB) and downloads
// missing tiles over Wi-Fi.
#include "map/tiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "hw/lcd.h"
#include "map/png.h"
#include "net/http.h"
#include "net/wifi.h"
#include "storage.h"

#define VW TILES_VIEW_W
#define VH TILES_VIEW_H
#define TILE 256
#define MAX_SLOTS 9 // a 320x288 view touches at most 3x3 tiles
#ifndef CACHE_DIR
#define CACHE_DIR "/meshtastic/tiles"
#endif

// Carto basemaps now need an API key; these are all free to use with a
// proper User-Agent and on-device caching.
static const tile_source_t sources[] = {
    {"OpenStreetMap", "tile.openstreetmap.org", "/%d/%ld/%ld.png", "osm", "\xC2\xA9 OpenStreetMap contributors", 19, false,
     false},
    {"OpenStreetMap Dark", "tile.openstreetmap.org", "/%d/%ld/%ld.png", "osm", "\xC2\xA9 OpenStreetMap contributors", 19,
     true, true},
    {"OpenTopoMap", "tile.opentopomap.org", "/%d/%ld/%ld.png", "otm", "\xC2\xA9 OpenStreetMap, SRTM | OpenTopoMap", 17,
     false, false},
    {"Humanitarian", "tile-a.openstreetmap.fr", "/hot/%d/%ld/%ld.png", "hot", "\xC2\xA9 OpenStreetMap contributors, HOT",
     19, false, false},
};
#define NSOURCES ((int)(sizeof(sources) / sizeof(sources[0])))

typedef enum {
    T_PENDING,    // cached file (maybe) present, needs decoding
    T_DONE,       // decoded into bg
    T_NEED_FETCH, // not cached, download when online
    T_FETCHING,
    T_FETCHED, // downloaded, decoded once the connection is closed
    T_MISSING, // outside the world / zoom range / server has no tile
    T_FAILED,  // download failed; retried on the next view change or reconnect
} tile_state_t;

typedef struct {
    int32_t tx, ty;
    uint8_t state;
} slot_t;

static uint8_t bg[VW * VH];
static uint16_t pal[256];
static int pal_n; // live entries
static bool pal_live[256];
static uint32_t ccache[1024]; // rgb565 << 16 | 0x100 | index

static int src_idx = -1; // source the buffer was decoded with
static int vz = -1;
static int32_t vox, voy;
static slot_t slots[MAX_SLOTS];
static int nslots;
static uint32_t changes;
static bool was_online;
static uint32_t gen; // bumped on source change so stale downloads are ignored

static struct {
    bool active;
    uint32_t gen;
    int z;
    int32_t tx, ty;
} fetch;

int tiles_source_count(void) { return NSOURCES; }

const tile_source_t *tiles_source(int index) { return index >= 0 && index < NSOURCES ? &sources[index] : NULL; }

const tile_source_t *tiles_active_source(void) { return tiles_source((int)g_settings.map_source - 1); }

uint32_t tiles_change_counter(void) { return changes; }

static int32_t floordiv(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// ------------------------------------------------------------------ palette

static void palette_reset(void) {
    const tile_source_t *s = tiles_active_source();
    memset(pal_live, 0, sizeof(pal_live));
    pal[0] = s && s->dark ? 0x10A2 /* C_BG */ : 0xDEFB /* light grey */;
    pal_live[0] = true;
    pal_n = 1;
    memset(ccache, 0, sizeof(ccache));
}

// Frees palette entries no longer used by any background pixel.
static void palette_gc(void) {
    static bool used[256];
    memset(used, 0, sizeof(used));
    used[0] = true;
    for (int i = 0; i < VW * VH; i++) used[bg[i]] = true;
    pal_n = 0;
    for (int i = 0; i < 256; i++) {
        pal_live[i] = pal_live[i] && used[i];
        pal_n += pal_live[i];
    }
    memset(ccache, 0, sizeof(ccache));
}

static uint8_t color_index(uint16_t c) {
    uint32_t h = ((c * 0x9E37u) >> 6) & 1023;
    uint32_t e = ccache[h];
    if ((e & 0x100) && (e >> 16) == c) return (uint8_t)e;
    int idx = -1, free_idx = -1;
    for (int i = 1; i < 256; i++) {
        if (!pal_live[i]) {
            if (free_idx < 0) free_idx = i;
        } else if (pal[i] == c) {
            idx = i;
            break;
        }
    }
    if (idx < 0 && free_idx >= 0) {
        idx = free_idx;
        pal[idx] = c;
        pal_live[idx] = true;
        pal_n++;
    }
    if (idx < 0) { // palette full: nearest colour
        int r = c >> 11, g = (c >> 5) & 63, b = c & 31, best = 1 << 30;
        for (int i = 1; i < 256; i++) {
            int dr = (pal[i] >> 11) - r, dg = ((pal[i] >> 5) & 63) - g, db = (pal[i] & 31) - b;
            int d = 4 * dr * dr + dg * dg + 4 * db * db;
            if (d < best) {
                best = d;
                idx = i;
            }
        }
    }
    ccache[h] = ((uint32_t)c << 16) | 0x100 | (uint32_t)idx;
    return (uint8_t)idx;
}

// Night version of a light map: inverts brightness but keeps the hue, so
// water stays blue and main roads stay orange. Plain white (minor roads) is
// kept lighter than the land so streets still stand out.
static uint16_t darken(uint16_t c) {
    int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
    int y = (77 * r + 150 * g + 29 * b) >> 8;
    int ny = y >= 248 ? 84 : 24 + (255 - y) * 3 / 4; // white roads become light grey lines
    int d = ny - y;
    r += d;
    g += d;
    b += d;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ------------------------------------------------------------------ view changes

static void clear_rect(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > VW) x1 = VW;
    if (y1 > VH) y1 = VH;
    for (int y = y0; y < y1; y++) memset(bg + y * VW + x0, 0, (size_t)(x1 > x0 ? x1 - x0 : 0));
}

static void shift_bg(int32_t dx, int32_t dy) { // content moves by (-dx, -dy)
    if (dx <= -VW || dx >= VW || dy <= -VH || dy >= VH) {
        memset(bg, 0, sizeof(bg));
        return;
    }
    int w = VW - abs((int)dx);
    int sx = dx > 0 ? (int)dx : 0, tx = dx > 0 ? 0 : (int)-dx;
    if (dy >= 0) {
        for (int y = 0; y < VH - dy; y++) memmove(bg + y * VW + tx, bg + (y + dy) * VW + sx, (size_t)w);
    } else {
        for (int y = VH - 1; y >= -dy; y--) memmove(bg + y * VW + tx, bg + (y + dy) * VW + sx, (size_t)w);
    }
    if (dy > 0) clear_rect(0, VH - (int)dy, VW, VH);
    if (dy < 0) clear_rect(0, 0, VW, (int)-dy);
    if (dx > 0) clear_rect(VW - (int)dx, 0, VW, VH);
    if (dx < 0) clear_rect(0, 0, (int)-dx, VH);
}

// Rescales bg in place for a one-level zoom so there is something to look at
// while the new tiles decode. up = zoom in.
static void scale_bg(bool up, int32_t oox, int32_t ooy, int32_t nox, int32_t noy) {
    static int16_t xmap[VW];
    static uint8_t tmp[VW];
    for (int x = 0; x < VW; x++) {
        int32_t s = up ? floordiv(nox + x, 2) - oox : (nox + x) * 2 - oox;
        xmap[x] = (int16_t)(s >= 0 && s < VW ? s : -1);
    }
#define SRC_ROW(y) (up ? floordiv(noy + (y), 2) - ooy : (noy + (y)) * 2 - ooy)
#define DO_ROW(y)                                                                                                       \
    do {                                                                                                                \
        int32_t sy = SRC_ROW(y);                                                                                        \
        if (sy < 0 || sy >= VH) {                                                                                       \
            memset(bg + (y) * VW, 0, VW);                                                                               \
        } else {                                                                                                        \
            const uint8_t *src = bg + sy * VW;                                                                          \
            for (int x = 0; x < VW; x++) tmp[x] = xmap[x] < 0 ? 0 : src[xmap[x]];                                       \
            memcpy(bg + (y) * VW, tmp, VW);                                                                             \
        }                                                                                                               \
    } while (0)
    // Rows whose source lies below are done top-down, rows whose source lies
    // above bottom-up, so no source row is overwritten before it is read.
    for (int y = 0; y < VH; y++) {
        if (SRC_ROW(y) > y) DO_ROW(y);
    }
    for (int y = VH - 1; y >= 0; y--) {
        if (SRC_ROW(y) < y) DO_ROW(y);
    }
    for (int y = 0; y < VH; y++) {
        if (SRC_ROW(y) == y) DO_ROW(y);
    }
#undef DO_ROW
#undef SRC_ROW
}

static slot_t *find_slot(int32_t tx, int32_t ty) {
    for (int i = 0; i < nslots; i++) {
        if (slots[i].tx == tx && slots[i].ty == ty) return &slots[i];
    }
    return NULL;
}

static int32_t world_tiles(int z) { return (int32_t)1 << z; }

static void build_slots(int z, int32_t ox, int32_t oy, bool same_zoom, int32_t pox, int32_t poy,
                        const slot_t *old, int nold) {
    int32_t tx0 = floordiv(ox, TILE), tx1 = floordiv(ox + VW - 1, TILE);
    int32_t ty0 = floordiv(oy, TILE), ty1 = floordiv(oy + VH - 1, TILE);
    const tile_source_t *s = tiles_active_source();
    nslots = 0;
    for (int32_t ty = ty0; ty <= ty1; ty++) {
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            slot_t *n = &slots[nslots++];
            n->tx = tx;
            n->ty = ty;
            n->state = T_PENDING;
            if (ty < 0 || ty >= world_tiles(z) || !s || z > s->max_zoom) {
                n->state = T_MISSING;
                continue;
            }
            if (!same_zoom) continue;
            const slot_t *o = NULL;
            for (int i = 0; i < nold; i++) {
                if (old[i].tx == tx && old[i].ty == ty) o = &old[i];
            }
            if (!o) continue;
            if (o->state == T_DONE) {
                // Still complete if the part now visible was visible before.
                int32_t x0 = tx * TILE > ox ? tx * TILE : ox, x1 = (tx + 1) * TILE < ox + VW ? (tx + 1) * TILE : ox + VW;
                int32_t y0 = ty * TILE > oy ? ty * TILE : oy, y1 = (ty + 1) * TILE < oy + VH ? (ty + 1) * TILE : oy + VH;
                if (x0 >= pox && x1 <= pox + VW && y0 >= poy && y1 <= poy + VH) n->state = T_DONE;
            } else if (o->state == T_FAILED) {
                n->state = T_NEED_FETCH;
            } else {
                n->state = o->state;
            }
        }
    }
}

void tiles_set_view(int z, int32_t ox, int32_t oy) {
    if (g_settings.map_source > NSOURCES) g_settings.map_source = 0;
    int src = (int)g_settings.map_source - 1;
    if (src != src_idx) {
        src_idx = src;
        gen++;
        vz = -1;
        palette_reset();
    }
    if (src < 0) return;
    if (z == vz && ox == vox && oy == voy) return;

    slot_t old[MAX_SLOTS];
    int nold = nslots;
    memcpy(old, slots, sizeof(old));
    bool same_zoom = z == vz;
    if (same_zoom) {
        shift_bg(ox - vox, oy - voy);
    } else if (vz >= 0 && (z == vz + 1 || z == vz - 1)) {
        scale_bg(z > vz, vox, voy, ox, oy);
    } else {
        palette_reset();
        memset(bg, 0, sizeof(bg));
    }
    build_slots(z, ox, oy, same_zoom, vox, voy, old, nold);
    vz = z;
    vox = ox;
    voy = oy;
    changes++;
}

// ------------------------------------------------------------------ decode / fetch

static void tile_path(char *out, size_t n, const tile_source_t *s, int z, int32_t tx, int32_t ty, bool file) {
    int32_t xw = ((tx % world_tiles(z)) + world_tiles(z)) % world_tiles(z);
    if (file) snprintf(out, n, CACHE_DIR "/%s/%d/%ld/%ld.png", s->dir, z, (long)xw, (long)ty);
    else snprintf(out, n, CACHE_DIR "/%s/%d/%ld", s->dir, z, (long)xw);
}

typedef struct {
    int sx0, sy0; // screen position of the tile's top-left pixel
    bool dark;
} draw_ctx_t;

static bool row_cb(void *vctx, int y, const uint16_t *row, int width) {
    draw_ctx_t *c = vctx;
    int sy = c->sy0 + y;
    if (sy < 0) return true;
    if (sy >= VH) return false;
    int x0 = c->sx0 < 0 ? -c->sx0 : 0;
    int x1 = c->sx0 + width > VW ? VW - c->sx0 : width;
    uint8_t *dst = bg + sy * VW + c->sx0;
    if (c->dark) {
        for (int x = x0; x < x1; x++) dst[x] = color_index(darken(row[x]));
    } else {
        for (int x = x0; x < x1; x++) dst[x] = color_index(row[x]);
    }
    return true;
}

static void clear_tile_area(const slot_t *t) {
    clear_rect((int)(t->tx * TILE - vox), (int)(t->ty * TILE - voy), (int)((t->tx + 1) * TILE - vox),
               (int)((t->ty + 1) * TILE - voy));
}

static uint32_t dist2(const slot_t *t) {
    int32_t cx = t->tx * TILE + TILE / 2 - (vox + VW / 2), cy = t->ty * TILE + TILE / 2 - (voy + VH / 2);
    return (uint32_t)(cx * cx + cy * cy);
}

static slot_t *nearest(tile_state_t st) {
    slot_t *best = NULL;
    for (int i = 0; i < nslots; i++) {
        if (slots[i].state == st && (!best || dist2(&slots[i]) < dist2(best))) best = &slots[i];
    }
    return best;
}

static void fetch_done(void *ctx, int result) {
    (void)ctx;
    fetch.active = false;
    if (fetch.gen != gen || fetch.z != vz) return;
    slot_t *t = find_slot(fetch.tx, fetch.ty);
    if (!t || t->state != T_FETCHING) return;
    t->state = result == 200 ? T_FETCHED : (result == 404 ? T_MISSING : T_FAILED);
    if (result != 200) printf("tiles: %d/%ld/%ld: %d\n", fetch.z, (long)fetch.tx, (long)fetch.ty, result);
    changes++;
}

void tiles_poll(void) {
    const tile_source_t *s = tiles_active_source();
    if (!s || vz < 0) return;

    bool online = wifi_online();
    if (online && !was_online) {
        for (int i = 0; i < nslots; i++) {
            if (slots[i].state == T_FAILED) slots[i].state = T_NEED_FETCH;
        }
        changes++;
    }
    was_online = online;

    // Order matters for RAM: the PNG decoder and a TLS connection do not
    // fit in the heap together, so cached tiles are decoded first, then all
    // missing ones are downloaded, then the connection is closed and the
    // downloads are decoded.
    char path[80];
    if (http_busy()) return;
    slot_t *t = nearest(T_PENDING);
    if (!t && !(online && nearest(T_NEED_FETCH))) {
        t = nearest(T_FETCHED);
        if (t) http_close_idle();
    }
    if (t) {
        tile_path(path, sizeof(path), s, vz, t->tx, t->ty, true);
        draw_ctx_t c = {(int)(t->tx * TILE - vox), (int)(t->ty * TILE - voy), s->invert};
        if (pal_n > 160) palette_gc();
        int r = png_decode_file(path, row_cb, &c);
        if (r == PNG_OK) {
            t->state = T_DONE;
        } else if (r == PNG_ERR_MEMORY) {
            if (http_connected()) http_close_idle(); // TLS buffers are the other big heap user
            return;
        } else if (r == PNG_ERR_OPEN) {
            t->state = T_NEED_FETCH;
        } else {
            printf("tiles: bad file %s (%d)\n", path, r);
            f_unlink(path);
            clear_tile_area(t);
            t->state = T_NEED_FETCH;
        }
        changes++;
        return;
    }

    if (!online || fetch.active) return;
    t = nearest(T_NEED_FETCH);
    if (!t) return;
    char dir[64], url[64];
    tile_path(dir, sizeof(dir), s, vz, t->tx, t->ty, false);
    tile_path(path, sizeof(path), s, vz, t->tx, t->ty, true);
    storage_mkdirs(dir);
    int32_t xw = ((t->tx % world_tiles(vz)) + world_tiles(vz)) % world_tiles(vz);
    snprintf(url, sizeof(url), s->path_fmt, vz, (long)xw, (long)t->ty);
    fetch.active = true;
    fetch.gen = gen;
    fetch.z = vz;
    fetch.tx = t->tx;
    fetch.ty = t->ty;
    if (http_get_file(s->host, true, url, path, fetch_done, NULL)) {
        t->state = T_FETCHING;
    } else {
        fetch.active = false;
        t->state = T_FAILED;
    }
    changes++;
}

bool tiles_draw(int y0) {
    if (src_idx < 0 || vz < 0) return false;
    const uint8_t *src = bg;
    uint16_t *dst = lcd_fb + y0 * LCD_W;
    for (int i = 0; i < VW * VH; i++) dst[i] = pal[src[i]];
    return true;
}

const char *tiles_status(void) {
    static char buf[40];
    if (!tiles_active_source() || vz < 0) return NULL;
    int need = 0, failed = 0, busy = 0;
    for (int i = 0; i < nslots; i++) {
        switch (slots[i].state) {
        case T_NEED_FETCH: need++; break;
        case T_FETCHING: busy++; break;
        case T_FAILED: failed++; break;
        case T_PENDING:
        case T_FETCHED: busy++; break;
        default: break;
        }
    }
    if (busy || (need && wifi_online())) {
        snprintf(buf, sizeof(buf), "Loading map tiles (%d)", busy + need);
        return buf;
    }
    if (need) return g_settings.wifi_enabled ? "Map tiles: waiting for Wi-Fi" : "Map tiles need Wi-Fi";
    if (failed) return "Some map tiles failed to download";
    return NULL;
}

int tiles_clear_cache(void) {
    int n = storage_rmtree(CACHE_DIR);
    gen++;
    vz = -1;
    nslots = 0;
    palette_reset();
    memset(bg, 0, sizeof(bg));
    changes++;
    return n;
}
