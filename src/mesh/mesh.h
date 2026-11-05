#ifndef MESH_MESH_H
#define MESH_MESH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "meshtastic/mesh.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/channel.pb.h"

#define MAX_NODES 200
#define MAX_MSGS 200
#define MAX_CHANNELS 8
#define MAX_TEXT_BYTES 200
#define NODENUM_BROADCAST 0xFFFFFFFFu

typedef struct {
    uint32_t num;
    char id[16];
    char long_name[40];
    char short_name[5];
    uint16_t hw_model;
    uint8_t role;
    uint8_t channel;
    bool has_user;
    bool has_pos;
    bool has_metrics;
    bool has_env;
    bool is_favorite;
    bool via_mqtt;
    bool has_pubkey;
    bool is_unmessagable;
    int8_t hops_away; // -1 = unknown
    int8_t battery;   // -1 = unknown, 101 = external power
    int32_t lat_i;
    int32_t lon_i;
    int32_t alt;
    uint32_t pos_time;
    uint32_t last_heard;    // epoch seconds as reported by the radio (0 = unknown)
    uint32_t last_heard_up; // local uptime seconds when last heard (0 = never this session)
    float snr;
    float voltage;
    float ch_util;
    float air_util;
    uint32_t uptime;
    float temperature;
    float humidity;
    float pressure;
} node_t;

typedef enum {
    MSG_RECEIVED = 0,
    MSG_QUEUED,    // handed to the BLE link
    MSG_SENT,      // accepted by the radio, waiting for an ACK
    MSG_ACKED,     // implicit ACK (a neighbour rebroadcast it)
    MSG_DELIVERED, // ACK from the destination
    MSG_FAILED,
} msg_status_t;

// Fixed 256-byte record so it can be stored verbatim on the SD card.
typedef struct {
    uint32_t seq; // local monotonically increasing sequence number
    uint32_t id;  // mesh packet id
    uint32_t from;
    uint32_t to;
    uint32_t time; // epoch seconds (0 = unknown)
    int16_t rssi;
    int8_t snr4; // SNR * 4
    uint8_t hops;
    uint8_t channel;
    uint8_t status;
    uint8_t error; // meshtastic_Routing_Error when failed
    uint8_t len;
    uint32_t file_index; // record index on SD (not meaningful in the file itself)
    char text[224];
} msg_t;

_Static_assert(sizeof(msg_t) == 256, "msg_t must stay 256 bytes");

// A conversation is either a channel (broadcast) or a direct message thread.
#define CONV_CHANNEL(ch) (0xFFFFFF00u | (uint32_t)(ch))
#define CONV_IS_CHANNEL(k) (((k) & 0xFFFFFF00u) == 0xFFFFFF00u)
#define CONV_CHANNEL_INDEX(k) ((k) & 0xFFu)

typedef struct {
    uint32_t key;
    int last_msg; // index into mesh_msg(), -1 if none
    int unread;
    uint32_t last_seq;
} conv_t;

typedef struct {
    // Link / config state
    bool link_up;
    bool config_complete;
    uint32_t config_nonce;
    uint32_t my_num;
    uint16_t reboot_count;
    char pio_env[40];

    bool has_metadata;
    meshtastic_DeviceMetadata metadata;
    bool has_lora;
    meshtastic_Config_LoRaConfig lora;
    bool has_device;
    meshtastic_Config_DeviceConfig device;
    bool has_position;
    meshtastic_Config_PositionConfig position;
    bool has_bluetooth;
    meshtastic_Config_BluetoothConfig bluetooth;
    bool has_display;
    meshtastic_Config_DisplayConfig display;

    meshtastic_Channel channels[MAX_CHANNELS];

    node_t nodes[MAX_NODES];
    int node_count;

    // Statistics
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_text;
    uint8_t queue_free;
    uint8_t queue_max;

    // Clock derived from timestamps reported by the radio
    uint32_t epoch_base;
    uint32_t epoch_base_up;

    // Last traceroute
    uint32_t tr_target;
    uint32_t tr_request_id;
    bool tr_pending;
    char tr_result[256];

    // Bumped whenever anything visible changes so the UI knows to redraw.
    uint32_t change_counter;
} mesh_state_t;

extern mesh_state_t g_mesh;

void mesh_init(void);
void mesh_poll(void);

// Called by the BLE link layer
void mesh_on_link_up(void);
void mesh_on_link_down(void);
void mesh_handle_fromradio(const uint8_t *data, size_t len);
bool mesh_wants_reads(void);

// Optional UI hook invoked for every newly received text message.
extern void (*mesh_on_new_message)(const msg_t *m);

// Messaging
int mesh_msg_count(void);
msg_t *mesh_msg(int idx); // 0 = oldest
bool mesh_send_text(uint32_t to, uint8_t channel, const char *text);
void mesh_clear_messages(void);

// Conversations
int mesh_conversations(conv_t *out, int max);
uint32_t mesh_msg_conv_key(const msg_t *m);
void mesh_mark_read(uint32_t conv_key);
int mesh_conv_unread(uint32_t conv_key);
void mesh_conv_title(uint32_t conv_key, char *out, size_t n);
int mesh_total_unread(void);

// Nodes
node_t *mesh_node_find(uint32_t num);
node_t *mesh_node_get(uint32_t num);
const node_t *mesh_my_node(void);
void mesh_node_label(uint32_t num, char *out, size_t n);     // short name or !xxxx
void mesh_node_longname(uint32_t num, char *out, size_t n);  // long name or !xxxxxxxx
uint32_t mesh_node_color(uint32_t num);                      // RGB888
bool mesh_node_is_online(const node_t *n);
int mesh_nodes_online(void);
// Distance (metres) and bearing (degrees) from our node; false if either position unknown.
bool mesh_node_distance(const node_t *n, float *metres, float *bearing);
void mesh_format_ago(uint32_t epoch, uint32_t up, char *out, size_t n);

// Channels
void mesh_channel_name(int idx, char *out, size_t n);
bool mesh_channel_active(int idx);
bool mesh_channel_url(char *out, size_t n);

// Remote / admin actions
bool mesh_send_traceroute(uint32_t dest);
bool mesh_request_position(uint32_t dest);
bool mesh_exchange_user_info(uint32_t dest);
bool mesh_admin_set_owner(const char *long_name, const char *short_name);
bool mesh_admin_set_lora(const meshtastic_Config_LoRaConfig *lora);
bool mesh_admin_set_device(const meshtastic_Config_DeviceConfig *dev);
bool mesh_admin_reboot(int seconds);
bool mesh_admin_set_favorite(uint32_t num, bool fav);
bool mesh_admin_remove_node(uint32_t num);

// Time helpers
uint32_t mesh_now(void);          // epoch seconds, or 0 if unknown
uint32_t mesh_uptime_s(void);

// Enum names
const char *mesh_region_name(int r);
const char *mesh_preset_name(int p);
const char *mesh_role_name(int r);
const char *mesh_hw_name(int hw);
const char *mesh_routing_error_name(int e);

#endif
