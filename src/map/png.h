#ifndef MAP_PNG_H
#define MAP_PNG_H

#include <stdbool.h>
#include <stdint.h>

#define PNG_MAX_WIDTH 256

enum {
    PNG_OK = 0,
    PNG_ERR_OPEN = -1,
    PNG_ERR_MEMORY = -2,
    PNG_ERR_FORMAT = -3,      // not a PNG / corrupt data
    PNG_ERR_UNSUPPORTED = -4, // interlaced or wider than PNG_MAX_WIDTH
};

// Called for each decoded row (RGB565, alpha blended over white).
// Return false to stop decoding early (png_decode_file then returns PNG_OK).
typedef bool (*png_row_cb)(void *ctx, int y, const uint16_t *row, int width);

// Streams a PNG file from the SD card. Needs ~40 KB of heap while running.
int png_decode_file(const char *path, png_row_cb cb, void *ctx);

#endif
