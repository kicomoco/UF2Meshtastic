// SD card persistence. All files live in /meshtastic on the card.
#include "storage.h"

#include <string.h>
#include <stdio.h>
#include "ff.h"

#define DIR_PATH "/meshtastic"
#define SETTINGS_PATH DIR_PATH "/settings.bin"
#define TLV_PATH DIR_PATH "/bt_keys.bin"
#define MSGS_PATH DIR_PATH "/messages.bin"
#define NODES_PATH DIR_PATH "/nodes.bin"

#define SETTINGS_MAGIC 0x4D534354u // "MSCT"
#define SETTINGS_VERSION 1
#define NODES_MAGIC 0x4E4F4445u // "NODE"
#define NODES_VERSION 1

app_settings_t g_settings;

static FATFS fs;
static bool mounted;

static void settings_defaults(void) {
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.magic = SETTINGS_MAGIC;
    g_settings.version = SETTINGS_VERSION;
    g_settings.auto_connect = true;
    g_settings.sound = true;
    g_settings.lcd_backlight = 200;
    g_settings.kbd_backlight = 0;
}

static bool read_file(const char *path, void *buf, UINT len, UINT *got) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    FRESULT r = f_read(&f, buf, len, got);
    f_close(&f);
    return r == FR_OK;
}

static bool write_file(const char *path, const void *buf, UINT len) {
    if (!mounted) return false;
    FIL f;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    UINT wr = 0;
    FRESULT r = f_write(&f, buf, len, &wr);
    f_close(&f);
    return r == FR_OK && wr == len;
}

// ---------------------------------------------------------------- TLV store

#define TLV_MAX 24
#define TLV_DATA 128

typedef struct {
    uint32_t tag;
    uint32_t len;
    uint8_t data[TLV_DATA];
} tlv_entry_t;

static tlv_entry_t tlv_entries[TLV_MAX];

static void tlv_persist(void) { write_file(TLV_PATH, tlv_entries, sizeof(tlv_entries)); }

static tlv_entry_t *tlv_find(uint32_t tag) {
    for (int i = 0; i < TLV_MAX; i++) {
        if (tlv_entries[i].tag == tag && tlv_entries[i].len) return &tlv_entries[i];
    }
    return NULL;
}

static int tlv_get_tag(void *ctx, uint32_t tag, uint8_t *buffer, uint32_t buffer_size) {
    (void)ctx;
    tlv_entry_t *e = tlv_find(tag);
    if (!e) return 0;
    if (!buffer) return (int)e->len;
    uint32_t n = e->len < buffer_size ? e->len : buffer_size;
    memcpy(buffer, e->data, n);
    return (int)n;
}

static int tlv_store_tag(void *ctx, uint32_t tag, const uint8_t *data, uint32_t size) {
    (void)ctx;
    if (size > TLV_DATA || size == 0) return 1;
    tlv_entry_t *e = tlv_find(tag);
    if (!e) {
        for (int i = 0; i < TLV_MAX; i++) {
            if (tlv_entries[i].len == 0) {
                e = &tlv_entries[i];
                break;
            }
        }
    }
    if (!e) return 1;
    if (e->tag == tag && e->len == size && memcmp(e->data, data, size) == 0) return 0;
    e->tag = tag;
    e->len = size;
    memcpy(e->data, data, size);
    tlv_persist();
    return 0;
}

static void tlv_delete_tag(void *ctx, uint32_t tag) {
    (void)ctx;
    tlv_entry_t *e = tlv_find(tag);
    if (!e) return;
    memset(e, 0, sizeof(*e));
    tlv_persist();
}

static const btstack_tlv_t tlv_impl = {
    .get_tag = tlv_get_tag,
    .store_tag = tlv_store_tag,
    .delete_tag = tlv_delete_tag,
};

const btstack_tlv_t *storage_tlv_instance(void) { return &tlv_impl; }
void *storage_tlv_context(void) { return tlv_entries; }

void storage_tlv_clear(void) {
    memset(tlv_entries, 0, sizeof(tlv_entries));
    tlv_persist();
}

// ---------------------------------------------------------------- general

bool storage_init(void) {
    settings_defaults();
    memset(tlv_entries, 0, sizeof(tlv_entries));

    mounted = f_mount(&fs, "", 1) == FR_OK;
    if (!mounted) return false;
    f_mkdir(DIR_PATH);

    app_settings_t s;
    UINT got = 0;
    if (read_file(SETTINGS_PATH, &s, sizeof(s), &got) && got == sizeof(s) && s.magic == SETTINGS_MAGIC &&
        s.version == SETTINGS_VERSION) {
        g_settings = s;
    }

    if (!read_file(TLV_PATH, tlv_entries, sizeof(tlv_entries), &got) || got != sizeof(tlv_entries)) {
        memset(tlv_entries, 0, sizeof(tlv_entries));
    }
    return true;
}

bool storage_available(void) { return mounted; }

void settings_save(void) { write_file(SETTINGS_PATH, &g_settings, sizeof(g_settings)); }

// ---------------------------------------------------------------- messages

bool storage_msg_append(msg_t *m) {
    if (!mounted) return false;
    FIL f;
    if (f_open(&f, MSGS_PATH, FA_OPEN_APPEND | FA_WRITE) != FR_OK) return false;
    m->file_index = (uint32_t)(f_size(&f) / sizeof(msg_t));
    UINT wr = 0;
    FRESULT r = f_write(&f, m, sizeof(*m), &wr);
    f_close(&f);
    return r == FR_OK && wr == sizeof(*m);
}

bool storage_msg_update(const msg_t *m) {
    if (!mounted) return false;
    FIL f;
    if (f_open(&f, MSGS_PATH, FA_OPEN_EXISTING | FA_WRITE) != FR_OK) return false;
    FSIZE_t off = (FSIZE_t)m->file_index * sizeof(msg_t);
    bool ok = false;
    if (off + sizeof(msg_t) <= f_size(&f) && f_lseek(&f, off) == FR_OK) {
        UINT wr = 0;
        ok = f_write(&f, m, sizeof(*m), &wr) == FR_OK && wr == sizeof(*m);
    }
    f_close(&f);
    return ok;
}

int storage_msg_load(msg_t *out, int max) {
    if (!mounted) return 0;
    FIL f;
    if (f_open(&f, MSGS_PATH, FA_READ) != FR_OK) return 0;
    uint32_t total = (uint32_t)(f_size(&f) / sizeof(msg_t));
    uint32_t start = total > (uint32_t)max ? total - (uint32_t)max : 0;
    int n = 0;
    if (f_lseek(&f, (FSIZE_t)start * sizeof(msg_t)) == FR_OK) {
        for (uint32_t i = start; i < total; i++) {
            UINT got = 0;
            if (f_read(&f, &out[n], sizeof(msg_t), &got) != FR_OK || got != sizeof(msg_t)) break;
            out[n].file_index = i;
            n++;
        }
    }
    f_close(&f);

    // Keep the file from growing forever: compact once it is 4x the RAM window.
    if (total > (uint32_t)max * 4) {
        if (f_open(&f, MSGS_PATH, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
            for (int i = 0; i < n; i++) {
                out[i].file_index = (uint32_t)i;
                UINT wr;
                f_write(&f, &out[i], sizeof(msg_t), &wr);
            }
            f_close(&f);
        }
    }
    return n;
}

void storage_msg_clear(void) {
    if (mounted) f_unlink(MSGS_PATH);
}

// ---------------------------------------------------------------- nodes

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t count;
} nodes_header_t;

void storage_nodes_save(const node_t *nodes, int count) {
    if (!mounted) return;
    FIL f;
    if (f_open(&f, NODES_PATH, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return;
    nodes_header_t h = {NODES_MAGIC, NODES_VERSION, sizeof(node_t), (uint32_t)count};
    UINT wr;
    f_write(&f, &h, sizeof(h), &wr);
    f_write(&f, nodes, sizeof(node_t) * (UINT)count, &wr);
    f_close(&f);
}

int storage_nodes_load(node_t *nodes, int max) {
    if (!mounted) return 0;
    FIL f;
    if (f_open(&f, NODES_PATH, FA_READ) != FR_OK) return 0;
    nodes_header_t h;
    UINT got = 0;
    int n = 0;
    if (f_read(&f, &h, sizeof(h), &got) == FR_OK && got == sizeof(h) && h.magic == NODES_MAGIC &&
        h.version == NODES_VERSION && h.record_size == sizeof(node_t)) {
        n = (int)(h.count < (uint32_t)max ? h.count : (uint32_t)max);
        if (f_read(&f, nodes, sizeof(node_t) * (UINT)n, &got) != FR_OK) n = 0;
        else n = (int)(got / sizeof(node_t));
    }
    f_close(&f);
    return n;
}
