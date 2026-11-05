// Small streaming PNG decoder for map tiles.
//
// The zlib stream is inflated with a 32 KB sliding window and scanlines are
// unfiltered and handed to the caller one at a time, so a 256x256 tile needs
// about 40 KB of RAM regardless of its colour type.
#include "map/png.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#ifdef PNG_HOST_TEST
#include <stdio.h>
typedef FILE *png_file_t;
static bool file_open(png_file_t *f, const char *path) { return (*f = fopen(path, "rb")) != NULL; }
static int file_read(png_file_t *f, uint8_t *buf, int n) { return (int)fread(buf, 1, (size_t)n, *f); }
static void file_close(png_file_t *f) { fclose(*f); }
#else
#include "ff.h"
typedef FIL png_file_t;
static bool file_open(png_file_t *f, const char *path) { return f_open(f, path, FA_READ) == FR_OK; }
static int file_read(png_file_t *f, uint8_t *buf, int n) {
    UINT got = 0;
    return f_read(f, buf, (UINT)n, &got) == FR_OK ? (int)got : 0;
}
static void file_close(png_file_t *f) { f_close(f); }
#endif

#define WSIZE 32768
#define WMASK (WSIZE - 1)
#define MAXBITS 15
#define MAX_ROW_BYTES (PNG_MAX_WIDTH * 8) // RGBA, 16 bits per sample

typedef struct {
    int16_t count[MAXBITS + 1];
    int16_t symbol[288];
} huffman_t;

typedef struct {
    png_file_t file;
    uint8_t in[1024];
    int in_pos, in_len;
    uint32_t chunk_left; // bytes left in the current IDAT chunk

    uint32_t bitbuf;
    int bitcnt;
    uint8_t window[WSIZE];
    uint32_t total_out;
    huffman_t lencode, distcode;

    int width, height, depth, color, bits_pp, filter_bpp, row_bytes;
    uint8_t row_a[MAX_ROW_BYTES + 1], row_b[MAX_ROW_BYTES + 1];
    uint8_t *cur, *prev;
    int row_pos, y;
    uint16_t palette[256];
    uint8_t pal_r[256], pal_g[256], pal_b[256], pal_a[256];
    bool has_trns;
    uint16_t trns_gray;
    uint16_t out[PNG_MAX_WIDTH];

    png_row_cb cb;
    void *ctx;
    jmp_buf jb;
} dec_t;

enum { JB_ERROR = 1, JB_DONE = 2, JB_UNSUPPORTED = 3 };

// ------------------------------------------------------------------ input

static uint8_t file_byte(dec_t *d) {
    if (d->in_pos == d->in_len) {
        d->in_len = file_read(&d->file, d->in, sizeof(d->in));
        d->in_pos = 0;
        if (d->in_len <= 0) longjmp(d->jb, JB_ERROR);
    }
    return d->in[d->in_pos++];
}

static uint32_t file_u32(dec_t *d) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | file_byte(d);
    return v;
}

static void file_skip(dec_t *d, uint32_t n) {
    while (n--) file_byte(d);
}

// Next byte of zlib data, moving across consecutive IDAT chunks.
static uint8_t idat_byte(dec_t *d) {
    while (d->chunk_left == 0) {
        file_skip(d, 4); // CRC of the previous chunk
        uint32_t len = file_u32(d);
        uint32_t type = file_u32(d);
        if (type != 0x49444154u) longjmp(d->jb, JB_ERROR); // "IDAT"
        d->chunk_left = len;
    }
    d->chunk_left--;
    return file_byte(d);
}

static int bits(dec_t *d, int need) {
    uint32_t val = d->bitbuf;
    while (d->bitcnt < need) {
        val |= (uint32_t)idat_byte(d) << d->bitcnt;
        d->bitcnt += 8;
    }
    d->bitbuf = val >> need;
    d->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

// ------------------------------------------------------------------ scanlines

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static inline uint16_t rgb565(int r, int g, int b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline int over_white(int c, int a) { return (c * a + 255 * (255 - a) + 127) / 255; }

static int sample(const uint8_t *p, int depth, int i) {
    switch (depth) {
    case 1: return (p[i >> 3] >> (7 - (i & 7))) & 1;
    case 2: return (p[i >> 2] >> ((3 - (i & 3)) * 2)) & 3;
    case 4: return (p[i >> 1] >> ((1 - (i & 1)) * 4)) & 15;
    case 8: return p[i];
    default: return (p[i * 2] << 8) | p[i * 2 + 1];
    }
}

static void emit_row(dec_t *d) {
    uint8_t *cur = d->cur + 1, *prev = d->prev + 1;
    int bpp = d->filter_bpp, n = d->row_bytes;
    switch (d->cur[0]) {
    case 0: break;
    case 1: for (int i = bpp; i < n; i++) cur[i] = (uint8_t)(cur[i] + cur[i - bpp]); break;
    case 2: for (int i = 0; i < n; i++) cur[i] = (uint8_t)(cur[i] + prev[i]); break;
    case 3:
        for (int i = 0; i < n; i++) cur[i] = (uint8_t)(cur[i] + (((i >= bpp ? cur[i - bpp] : 0) + prev[i]) >> 1));
        break;
    case 4:
        for (int i = 0; i < n; i++)
            cur[i] = (uint8_t)(cur[i] + paeth(i >= bpp ? cur[i - bpp] : 0, prev[i], i >= bpp ? prev[i - bpp] : 0));
        break;
    default: longjmp(d->jb, JB_ERROR);
    }

    int w = d->width, depth = d->depth;
    int step = depth == 16 ? 2 : 1; // byte stride between samples for 8/16-bit data
    switch (d->color) {
    case 3:
        for (int x = 0; x < w; x++) d->out[x] = d->palette[sample(cur, depth, x)];
        break;
    case 0: {
        int maxv = (1 << depth) - 1;
        for (int x = 0; x < w; x++) {
            int s = sample(cur, depth, x);
            int v = depth == 16 ? s >> 8 : s * 255 / maxv;
            if (d->has_trns && s == d->trns_gray) v = 255;
            d->out[x] = rgb565(v, v, v);
        }
        break;
    }
    case 2:
        for (int x = 0; x < w; x++) {
            const uint8_t *p = cur + x * 3 * step;
            d->out[x] = rgb565(p[0], p[step], p[2 * step]);
        }
        break;
    case 4:
        for (int x = 0; x < w; x++) {
            const uint8_t *p = cur + x * 2 * step;
            int v = over_white(p[0], p[step]);
            d->out[x] = rgb565(v, v, v);
        }
        break;
    case 6:
        for (int x = 0; x < w; x++) {
            const uint8_t *p = cur + x * 4 * step;
            int a = p[3 * step];
            d->out[x] = rgb565(over_white(p[0], a), over_white(p[step], a), over_white(p[2 * step], a));
        }
        break;
    }
    if (!d->cb(d->ctx, d->y, d->out, w)) longjmp(d->jb, JB_DONE);
    if (++d->y == d->height) longjmp(d->jb, JB_DONE);
    uint8_t *t = d->prev;
    d->prev = d->cur;
    d->cur = t;
}

static inline void put(dec_t *d, uint8_t b) {
    d->window[d->total_out++ & WMASK] = b;
    d->cur[d->row_pos++] = b;
    if (d->row_pos == d->row_bytes + 1) {
        d->row_pos = 0;
        emit_row(d);
    }
}

// ------------------------------------------------------------------ inflate

static int decode(dec_t *d, const huffman_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= bits(d, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    longjmp(d->jb, JB_ERROR);
}

static void construct(dec_t *d, huffman_t *h, const uint8_t *length, int n) {
    int16_t offs[MAXBITS + 1];
    memset(h->count, 0, sizeof(h->count));
    for (int s = 0; s < n; s++) h->count[length[s]]++;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) longjmp(d->jb, JB_ERROR); // over-subscribed
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    for (int s = 0; s < n; s++) {
        if (length[s]) h->symbol[offs[length[s]]++] = (int16_t)s;
    }
}

static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void codes(dec_t *d) {
    for (;;) {
        int sym = decode(d, &d->lencode);
        if (sym < 256) {
            put(d, (uint8_t)sym);
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) longjmp(d->jb, JB_ERROR);
            int len = len_base[sym] + bits(d, len_extra[sym]);
            int ds = decode(d, &d->distcode);
            if (ds >= 30) longjmp(d->jb, JB_ERROR);
            uint32_t dist = dist_base[ds] + (uint32_t)bits(d, dist_extra[ds]);
            if (dist > d->total_out) longjmp(d->jb, JB_ERROR);
            while (len--) put(d, d->window[(d->total_out - dist) & WMASK]);
        }
    }
}

static void stored(dec_t *d) {
    d->bitbuf = 0;
    d->bitcnt = 0;
    int len = idat_byte(d);
    len |= idat_byte(d) << 8;
    int nlen = idat_byte(d);
    nlen |= idat_byte(d) << 8;
    if (len != (~nlen & 0xFFFF)) longjmp(d->jb, JB_ERROR);
    while (len--) put(d, idat_byte(d));
}

static void fixed(dec_t *d) {
    uint8_t lengths[288];
    int s = 0;
    for (; s < 144; s++) lengths[s] = 8;
    for (; s < 256; s++) lengths[s] = 9;
    for (; s < 280; s++) lengths[s] = 7;
    for (; s < 288; s++) lengths[s] = 8;
    construct(d, &d->lencode, lengths, 288);
    for (s = 0; s < 30; s++) lengths[s] = 5;
    construct(d, &d->distcode, lengths, 30);
    codes(d);
}

static void dynamic(dec_t *d) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lengths[320];
    int nlen = bits(d, 5) + 257;
    int ndist = bits(d, 5) + 1;
    int ncode = bits(d, 4) + 4;
    if (nlen > 286 || ndist > 30) longjmp(d->jb, JB_ERROR);
    int i = 0;
    for (; i < ncode; i++) lengths[order[i]] = (uint8_t)bits(d, 3);
    for (; i < 19; i++) lengths[order[i]] = 0;
    construct(d, &d->lencode, lengths, 19);

    i = 0;
    while (i < nlen + ndist) {
        int sym = decode(d, &d->lencode);
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        int len = 0, rep;
        if (sym == 16) {
            if (i == 0) longjmp(d->jb, JB_ERROR);
            len = lengths[i - 1];
            rep = 3 + bits(d, 2);
        } else if (sym == 17) {
            rep = 3 + bits(d, 3);
        } else {
            rep = 11 + bits(d, 7);
        }
        if (i + rep > nlen + ndist) longjmp(d->jb, JB_ERROR);
        while (rep--) lengths[i++] = (uint8_t)len;
    }
    if (lengths[256] == 0) longjmp(d->jb, JB_ERROR);
    construct(d, &d->lencode, lengths, nlen);
    construct(d, &d->distcode, lengths + nlen, ndist);
    codes(d);
}

static void inflate_stream(dec_t *d) {
    int cmf = idat_byte(d), flg = idat_byte(d);
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 || (flg & 0x20)) longjmp(d->jb, JB_ERROR);
    int last;
    do {
        last = bits(d, 1);
        int type = bits(d, 2);
        if (type == 0) stored(d);
        else if (type == 1) fixed(d);
        else if (type == 2) dynamic(d);
        else longjmp(d->jb, JB_ERROR);
    } while (!last);
    longjmp(d->jb, JB_ERROR); // data ended before the last row
}

// ------------------------------------------------------------------ chunks

static void parse_and_decode(dec_t *d) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    for (int i = 0; i < 8; i++) {
        if (file_byte(d) != sig[i]) longjmp(d->jb, JB_ERROR);
    }
    bool have_header = false;
    for (;;) {
        uint32_t len = file_u32(d);
        uint32_t type = file_u32(d);
        if (type == 0x49484452u) { // IHDR
            d->width = (int)file_u32(d);
            d->height = (int)file_u32(d);
            d->depth = file_byte(d);
            d->color = file_byte(d);
            int comp = file_byte(d), filt = file_byte(d), interlace = file_byte(d);
            file_skip(d, len - 13 + 4);
            static const uint8_t channels[7] = {1, 0, 3, 1, 2, 0, 4};
            if (d->color > 6 || !channels[d->color] || comp || filt) longjmp(d->jb, JB_ERROR);
            if (interlace || d->width < 1 || d->width > PNG_MAX_WIDTH || d->height < 1) longjmp(d->jb, JB_UNSUPPORTED);
            if (d->depth != 1 && d->depth != 2 && d->depth != 4 && d->depth != 8 && d->depth != 16)
                longjmp(d->jb, JB_ERROR);
            d->bits_pp = channels[d->color] * d->depth;
            d->filter_bpp = d->bits_pp < 8 ? 1 : d->bits_pp / 8;
            d->row_bytes = (d->width * d->bits_pp + 7) / 8;
            have_header = true;
        } else if (type == 0x504C5445u) { // PLTE
            int n = (int)len / 3;
            for (int i = 0; i < n && i < 256; i++) {
                d->pal_r[i] = file_byte(d);
                d->pal_g[i] = file_byte(d);
                d->pal_b[i] = file_byte(d);
            }
            file_skip(d, len - (uint32_t)(n < 256 ? n : 256) * 3 + 4);
        } else if (type == 0x74524E53u) { // tRNS
            if (d->color == 3) {
                for (uint32_t i = 0; i < len; i++) {
                    uint8_t a = file_byte(d);
                    if (i < 256) d->pal_a[i] = a;
                }
                file_skip(d, 4);
            } else if (d->color == 0 && len >= 2) {
                d->trns_gray = (uint16_t)(file_byte(d) << 8);
                d->trns_gray |= file_byte(d);
                d->has_trns = true;
                file_skip(d, len - 2 + 4);
            } else {
                file_skip(d, len + 4);
            }
        } else if (type == 0x49444154u) { // IDAT
            if (!have_header) longjmp(d->jb, JB_ERROR);
            for (int i = 0; i < 256; i++)
                d->palette[i] = rgb565(over_white(d->pal_r[i], d->pal_a[i]), over_white(d->pal_g[i], d->pal_a[i]),
                                       over_white(d->pal_b[i], d->pal_a[i]));
            d->chunk_left = len;
            inflate_stream(d);
        } else if (type == 0x49454E44u) { // IEND
            longjmp(d->jb, JB_ERROR);
        } else {
            file_skip(d, len + 4);
        }
    }
}

int png_decode_file(const char *path, png_row_cb cb, void *ctx) {
    dec_t *d = calloc(1, sizeof(dec_t));
    if (!d) return PNG_ERR_MEMORY;
    if (!file_open(&d->file, path)) {
        free(d);
        return PNG_ERR_OPEN;
    }
    memset(d->pal_a, 255, sizeof(d->pal_a));
    d->cb = cb;
    d->ctx = ctx;
    d->cur = d->row_a;
    d->prev = d->row_b;
    int r = setjmp(d->jb);
    if (r == 0) parse_and_decode(d);
    file_close(&d->file);
    free(d);
    return r == JB_DONE ? PNG_OK : r == JB_UNSUPPORTED ? PNG_ERR_UNSUPPORTED : PNG_ERR_FORMAT;
}
