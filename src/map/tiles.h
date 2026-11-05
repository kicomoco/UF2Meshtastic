#ifndef MAP_TILES_H
#define MAP_TILES_H

#include <stdbool.h>
#include <stdint.h>

// Slippy-map tile background for the Map tab. Tiles are downloaded over
// Wi-Fi into /meshtastic/tiles on the SD card and reused offline.

#define TILES_VIEW_W 320
#define TILES_VIEW_H 288

typedef struct {
    const char *name;
    const char *host;
    const char *path_fmt; // printf format taking z, x, y
    const char *dir;      // cache directory name
    const char *attribution;
    uint8_t max_zoom;
    bool dark;   // UI uses light text/boxes and a dark empty colour
    bool invert; // recolour light tiles for night use
} tile_source_t;

int tiles_source_count(void);
const tile_source_t *tiles_source(int index);
// Selected source from g_settings.map_source, or NULL when tiles are off.
const tile_source_t *tiles_active_source(void);

// View: zoom level and the world pixel at the top-left of the view.
void tiles_set_view(int z, int32_t ox, int32_t oy);
// Blits the background into the framebuffer at row y0 (TILES_VIEW_H rows).
// Returns false if tiles are disabled (nothing drawn).
bool tiles_draw(int y0);
// Decodes / downloads tiles; call from the main loop.
void tiles_poll(void);
// Short status for the map overlay, or NULL when everything is shown.
const char *tiles_status(void);
uint32_t tiles_change_counter(void);
// Removes all cached tiles. Returns the number of files deleted.
int tiles_clear_cache(void);

#endif
