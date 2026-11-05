#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "btstack_tlv.h"
#include "mesh/mesh.h"

typedef struct {
    uint32_t magic;
    uint16_t version;
    bool has_last_device;
    uint8_t last_addr[6];
    uint8_t last_addr_type;
    char last_name[32];
    bool auto_connect;
    bool sound;
    bool imperial;
    uint8_t lcd_backlight;
    uint8_t kbd_backlight;
    int16_t tz_minutes;
} app_settings_t;

extern app_settings_t g_settings;

bool storage_init(void);
bool storage_available(void);

void settings_save(void);

// BTstack TLV backed by RAM + SD file
const btstack_tlv_t *storage_tlv_instance(void);
void *storage_tlv_context(void);
void storage_tlv_clear(void);

// Message log
bool storage_msg_append(msg_t *m);
bool storage_msg_update(const msg_t *m);
int storage_msg_load(msg_t *out, int max);
void storage_msg_clear(void);

// Node cache
void storage_nodes_save(const node_t *nodes, int count);
int storage_nodes_load(node_t *nodes, int max);

#endif
