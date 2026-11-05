// Meshtastic client protocol: decodes FromRadio, keeps the node / message
// database and builds ToRadio packets (texts, admin, traceroute, ...).
#include "mesh/mesh.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/rand.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/admin.pb.h"
#include "meshtastic/apponly.pb.h"
#include "meshtastic/telemetry.pb.h"
#include "meshtastic/portnums.pb.h"

#include "mesh/ble_link.h"
#include "storage.h"
#include "log.h"

mesh_state_t g_mesh;
void (*mesh_on_new_message)(const msg_t *m);

static msg_t msgs[MAX_MSGS];
static int msg_count;
static uint32_t next_seq = 1;

#define MAX_UNREAD 48
static struct {
    uint32_t key;
    int count;
} unread[MAX_UNREAD];

static bool nodes_dirty;
static uint32_t nodes_save_at;
static uint32_t next_heartbeat;

// Scratch buffers kept static: FromRadio / ToRadio structs are large.
static meshtastic_FromRadio fr;
static meshtastic_ToRadio tr;
static uint8_t txbuf[meshtastic_ToRadio_size + 8];

uint32_t mesh_uptime_s(void) { return to_ms_since_boot(get_absolute_time()) / 1000; }

uint32_t mesh_now(void) {
    if (!g_mesh.epoch_base) return 0;
    return g_mesh.epoch_base + (mesh_uptime_s() - g_mesh.epoch_base_up);
}

static void set_clock(uint32_t epoch) {
    if (epoch < 1600000000u || epoch > 4000000000u) return;
    g_mesh.epoch_base = epoch;
    g_mesh.epoch_base_up = mesh_uptime_s();
}

static void changed(void) { g_mesh.change_counter++; }

// ------------------------------------------------------------------ nodes

node_t *mesh_node_find(uint32_t num) {
    for (int i = 0; i < g_mesh.node_count; i++) {
        if (g_mesh.nodes[i].num == num) return &g_mesh.nodes[i];
    }
    return NULL;
}

node_t *mesh_node_get(uint32_t num) {
    if (num == 0 || num == NODENUM_BROADCAST) return NULL;
    node_t *n = mesh_node_find(num);
    if (n) return n;
    if (g_mesh.node_count < MAX_NODES) {
        n = &g_mesh.nodes[g_mesh.node_count++];
    } else {
        // Evict the least recently heard non-favourite node
        int victim = -1;
        for (int i = 0; i < g_mesh.node_count; i++) {
            node_t *c = &g_mesh.nodes[i];
            if (c->is_favorite || c->num == g_mesh.my_num) continue;
            if (victim < 0 || c->last_heard < g_mesh.nodes[victim].last_heard) victim = i;
        }
        if (victim < 0) return NULL;
        n = &g_mesh.nodes[victim];
    }
    memset(n, 0, sizeof(*n));
    n->num = num;
    n->hops_away = -1;
    n->battery = -1;
    snprintf(n->id, sizeof(n->id), "!%08lx", (unsigned long)num);
    nodes_dirty = true;
    return n;
}

const node_t *mesh_my_node(void) { return g_mesh.my_num ? mesh_node_find(g_mesh.my_num) : NULL; }

void mesh_node_label(uint32_t num, char *out, size_t n) {
    const node_t *nd = mesh_node_find(num);
    if (nd && nd->short_name[0]) {
        snprintf(out, n, "%s", nd->short_name);
    } else {
        snprintf(out, n, "%04lx", (unsigned long)(num & 0xFFFF));
    }
}

void mesh_node_longname(uint32_t num, char *out, size_t n) {
    const node_t *nd = mesh_node_find(num);
    if (nd && nd->long_name[0]) {
        snprintf(out, n, "%s", nd->long_name);
    } else {
        snprintf(out, n, "Meshtastic %04lx", (unsigned long)(num & 0xFFFF));
    }
}

uint32_t mesh_node_color(uint32_t num) { return num & 0xFFFFFF; }

bool mesh_node_is_online(const node_t *n) {
    uint32_t now = mesh_now();
    if (now && n->last_heard) return now - n->last_heard < 2 * 3600;
    if (n->last_heard_up) return mesh_uptime_s() - n->last_heard_up < 2 * 3600;
    return false;
}

int mesh_nodes_online(void) {
    int c = 0;
    for (int i = 0; i < g_mesh.node_count; i++) {
        if (g_mesh.nodes[i].num != g_mesh.my_num && mesh_node_is_online(&g_mesh.nodes[i])) c++;
    }
    return c;
}

bool mesh_node_distance(const node_t *n, float *metres, float *bearing) {
    const node_t *me = mesh_my_node();
    if (!me || !me->has_pos || !n->has_pos || n == me) return false;
    double la1 = me->lat_i * 1e-7 * M_PI / 180, lo1 = me->lon_i * 1e-7 * M_PI / 180;
    double la2 = n->lat_i * 1e-7 * M_PI / 180, lo2 = n->lon_i * 1e-7 * M_PI / 180;
    double dla = la2 - la1, dlo = lo2 - lo1;
    double a = sin(dla / 2) * sin(dla / 2) + cos(la1) * cos(la2) * sin(dlo / 2) * sin(dlo / 2);
    if (metres) *metres = (float)(6371000.0 * 2 * atan2(sqrt(a), sqrt(1 - a)));
    if (bearing) {
        double y = sin(dlo) * cos(la2);
        double x = cos(la1) * sin(la2) - sin(la1) * cos(la2) * cos(dlo);
        double b = atan2(y, x) * 180 / M_PI;
        *bearing = (float)fmod(b + 360.0, 360.0);
    }
    return true;
}

void mesh_format_ago(uint32_t epoch, uint32_t up, char *out, size_t n) {
    uint32_t age;
    uint32_t now = mesh_now();
    if (now && epoch && epoch <= now + 60) {
        age = now > epoch ? now - epoch : 0;
    } else if (up) {
        age = mesh_uptime_s() - up;
    } else {
        snprintf(out, n, "never");
        return;
    }
    if (age < 60) snprintf(out, n, "now");
    else if (age < 3600) snprintf(out, n, "%lum", (unsigned long)(age / 60));
    else if (age < 86400) snprintf(out, n, "%luh", (unsigned long)(age / 3600));
    else snprintf(out, n, "%lud", (unsigned long)(age / 86400));
}

static void apply_user(node_t *n, const meshtastic_User *u) {
    snprintf(n->id, sizeof(n->id), "%s", u->id);
    snprintf(n->long_name, sizeof(n->long_name), "%s", u->long_name);
    snprintf(n->short_name, sizeof(n->short_name), "%s", u->short_name);
    n->hw_model = (uint16_t)u->hw_model;
    n->role = (uint8_t)u->role;
    n->has_pubkey = u->public_key.size == 32;
    n->is_unmessagable = u->has_is_unmessagable && u->is_unmessagable;
    n->has_user = true;
}

static void apply_position(node_t *n, const meshtastic_Position *p) {
    if (!p->has_latitude_i || !p->has_longitude_i || (p->latitude_i == 0 && p->longitude_i == 0)) return;
    n->lat_i = p->latitude_i;
    n->lon_i = p->longitude_i;
    n->alt = p->has_altitude ? p->altitude : 0;
    n->pos_time = p->time;
    n->has_pos = true;
}

static void apply_metrics(node_t *n, const meshtastic_DeviceMetrics *m) {
    if (m->has_battery_level) n->battery = (int8_t)(m->battery_level > 101 ? 101 : m->battery_level);
    if (m->has_voltage) n->voltage = m->voltage;
    if (m->has_channel_utilization) n->ch_util = m->channel_utilization;
    if (m->has_air_util_tx) n->air_util = m->air_util_tx;
    if (m->has_uptime_seconds) n->uptime = m->uptime_seconds;
    n->has_metrics = true;
}

static void handle_node_info(const meshtastic_NodeInfo *ni) {
    node_t *n = mesh_node_get(ni->num);
    if (!n) return;
    if (ni->has_user) apply_user(n, &ni->user);
    if (ni->has_position) apply_position(n, &ni->position);
    if (ni->has_device_metrics) apply_metrics(n, &ni->device_metrics);
    if (ni->last_heard) n->last_heard = ni->last_heard;
    n->snr = ni->snr;
    n->channel = ni->channel;
    n->via_mqtt = ni->via_mqtt;
    n->hops_away = ni->has_hops_away ? (int8_t)ni->hops_away : -1;
    n->is_favorite = ni->is_favorite;
    if (ni->num == g_mesh.my_num) {
        n->hops_away = 0;
        set_clock(ni->last_heard);
    }
    nodes_dirty = true;
}

// ------------------------------------------------------------------ messages

int mesh_msg_count(void) { return msg_count; }
msg_t *mesh_msg(int idx) { return idx >= 0 && idx < msg_count ? &msgs[idx] : NULL; }

uint32_t mesh_msg_conv_key(const msg_t *m) {
    if (m->to == NODENUM_BROADCAST) return CONV_CHANNEL(m->channel);
    return m->from == g_mesh.my_num || m->from == 0 ? m->to : m->from;
}

static msg_t *add_msg(const msg_t *m) {
    if (msg_count == MAX_MSGS) {
        memmove(&msgs[0], &msgs[1], sizeof(msg_t) * (MAX_MSGS - 1));
        msg_count--;
    }
    msg_t *d = &msgs[msg_count++];
    *d = *m;
    d->seq = next_seq++;
    storage_msg_append(d);
    changed();
    return d;
}

static msg_t *find_my_msg(uint32_t id) {
    for (int i = msg_count - 1; i >= 0; i--) {
        if (msgs[i].id == id && msgs[i].status != MSG_RECEIVED) return &msgs[i];
    }
    return NULL;
}

static void set_msg_status(msg_t *m, msg_status_t st, uint8_t err) {
    // Never downgrade a delivered message to merely acked.
    if (m->status == MSG_DELIVERED && st != MSG_FAILED) return;
    m->status = st;
    m->error = err;
    storage_msg_update(m);
    changed();
}

void mesh_clear_messages(void) {
    msg_count = 0;
    memset(unread, 0, sizeof(unread));
    storage_msg_clear();
    changed();
}

static void unread_add(uint32_t key) {
    int free_slot = -1;
    for (int i = 0; i < MAX_UNREAD; i++) {
        if (unread[i].count && unread[i].key == key) {
            unread[i].count++;
            return;
        }
        if (!unread[i].count && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        unread[free_slot].key = key;
        unread[free_slot].count = 1;
    }
}

void mesh_mark_read(uint32_t key) {
    for (int i = 0; i < MAX_UNREAD; i++) {
        if (unread[i].count && unread[i].key == key) {
            unread[i].count = 0;
            changed();
        }
    }
}

int mesh_conv_unread(uint32_t key) {
    for (int i = 0; i < MAX_UNREAD; i++) {
        if (unread[i].count && unread[i].key == key) return unread[i].count;
    }
    return 0;
}

int mesh_total_unread(void) {
    int t = 0;
    for (int i = 0; i < MAX_UNREAD; i++) t += unread[i].count;
    return t;
}

void mesh_conv_title(uint32_t key, char *out, size_t n) {
    if (CONV_IS_CHANNEL(key)) {
        mesh_channel_name((int)CONV_CHANNEL_INDEX(key), out, n);
    } else {
        mesh_node_longname(key, out, n);
    }
}

int mesh_conversations(conv_t *out, int max) {
    int n = 0;
    // Active channels always appear, like the Android app.
    for (int ch = 0; ch < MAX_CHANNELS && n < max; ch++) {
        if (!mesh_channel_active(ch)) continue;
        out[n].key = CONV_CHANNEL(ch);
        out[n].last_msg = -1;
        out[n].last_seq = 0;
        out[n].unread = 0;
        n++;
    }
    for (int i = 0; i < msg_count; i++) {
        uint32_t key = mesh_msg_conv_key(&msgs[i]);
        int j;
        for (j = 0; j < n; j++) {
            if (out[j].key == key) break;
        }
        if (j == n) {
            if (n >= max) continue;
            out[n].key = key;
            n++;
        }
        out[j].last_msg = i;
        out[j].last_seq = msgs[i].seq;
    }
    for (int j = 0; j < n; j++) out[j].unread = mesh_conv_unread(out[j].key);
    // Most recent first; channels without messages keep index order at the end.
    for (int a = 1; a < n; a++) {
        conv_t t = out[a];
        int b = a - 1;
        while (b >= 0 && out[b].last_seq < t.last_seq) {
            out[b + 1] = out[b];
            b--;
        }
        out[b + 1] = t;
    }
    return n;
}

// ------------------------------------------------------------------ channels

bool mesh_channel_active(int idx) {
    if (idx < 0 || idx >= MAX_CHANNELS) return false;
    return g_mesh.channels[idx].role != meshtastic_Channel_Role_DISABLED;
}

void mesh_channel_name(int idx, char *out, size_t n) {
    if (idx < 0 || idx >= MAX_CHANNELS) {
        snprintf(out, n, "Ch %d", idx);
        return;
    }
    const meshtastic_Channel *c = &g_mesh.channels[idx];
    if (c->has_settings && c->settings.name[0]) {
        snprintf(out, n, "%s", c->settings.name);
    } else if (c->role == meshtastic_Channel_Role_PRIMARY || (idx == 0 && c->role == meshtastic_Channel_Role_DISABLED)) {
        // Unnamed primary channel is displayed with the modem preset name.
        const char *p = g_mesh.has_lora && g_mesh.lora.use_preset ? mesh_preset_name(g_mesh.lora.modem_preset) : "Primary";
        snprintf(out, n, "%s", p);
    } else {
        snprintf(out, n, "Channel %d", idx);
    }
}

static void base64url(const uint8_t *in, size_t len, char *out, size_t outn) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int bytes = 1;
        if (i + 1 < len) { v |= (uint32_t)in[i + 1] << 8; bytes++; }
        if (i + 2 < len) { v |= in[i + 2]; bytes++; }
        for (int k = 0; k <= bytes && o + 1 < outn; k++) out[o++] = tbl[(v >> (18 - 6 * k)) & 63];
    }
    out[o] = 0;
}

bool mesh_channel_url(char *out, size_t n) {
    static meshtastic_ChannelSet cs;
    static uint8_t buf[meshtastic_ChannelSet_size];
    memset(&cs, 0, sizeof(cs));
    for (int i = 0; i < MAX_CHANNELS; i++) {
        const meshtastic_Channel *c = &g_mesh.channels[i];
        if (c->role == meshtastic_Channel_Role_DISABLED || !c->has_settings) continue;
        cs.settings[cs.settings_count++] = c->settings;
    }
    if (!cs.settings_count) return false;
    if (g_mesh.has_lora) {
        cs.has_lora_config = true;
        cs.lora_config = g_mesh.lora;
    }
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, meshtastic_ChannelSet_fields, &cs)) return false;
    int p = snprintf(out, n, "https://meshtastic.org/e/#");
    if (p < 0 || (size_t)p >= n) return false;
    base64url(buf, os.bytes_written, out + p, n - (size_t)p);
    return true;
}

// ------------------------------------------------------------------ sending

static uint32_t new_packet_id(void) {
    uint32_t id;
    do {
        id = get_rand_32();
    } while (id == 0);
    return id;
}

static uint8_t hop_limit(void) {
    if (g_mesh.has_lora && g_mesh.lora.hop_limit) return (uint8_t)g_mesh.lora.hop_limit;
    return 3;
}

static bool send_toradio(void) {
    pb_ostream_t os = pb_ostream_from_buffer(txbuf, sizeof(txbuf));
    if (!pb_encode(&os, meshtastic_ToRadio_fields, &tr)) {
        app_log("Encode failed: %s", PB_GET_ERROR(&os));
        return false;
    }
    return ble_link_send(txbuf, (uint16_t)os.bytes_written);
}

// Sends a data packet; returns the packet id (0 on failure).
static uint32_t send_data(uint32_t to, uint8_t channel, meshtastic_PortNum port, const void *payload, size_t len,
                          bool want_ack, bool want_response) {
    if (!g_mesh.link_up || len > sizeof(tr.packet.decoded.payload.bytes)) return 0;
    memset(&tr, 0, sizeof(tr));
    tr.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *p = &tr.packet;
    p->to = to;
    p->channel = channel;
    p->id = new_packet_id();
    p->hop_limit = hop_limit();
    p->want_ack = want_ack;
    p->priority = meshtastic_MeshPacket_Priority_RELIABLE;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = port;
    p->decoded.want_response = want_response;
    memcpy(p->decoded.payload.bytes, payload, len);
    p->decoded.payload.size = (pb_size_t)len;
    if (!send_toradio()) return 0;
    g_mesh.tx_packets++;
    return p->id;
}

static uint32_t send_proto(uint32_t to, uint8_t channel, meshtastic_PortNum port, const pb_msgdesc_t *fields,
                           const void *msg, bool want_ack, bool want_response) {
    static uint8_t pl[sizeof(((meshtastic_Data *)0)->payload.bytes)];
    pb_ostream_t os = pb_ostream_from_buffer(pl, sizeof(pl));
    if (!pb_encode(&os, fields, msg)) {
        app_log("Encode failed: %s", PB_GET_ERROR(&os));
        return 0;
    }
    return send_data(to, channel, port, pl, os.bytes_written, want_ack, want_response);
}

bool mesh_send_text(uint32_t to, uint8_t channel, const char *text) {
    size_t len = strlen(text);
    if (len == 0) return false;
    if (len > MAX_TEXT_BYTES) len = MAX_TEXT_BYTES;
    uint32_t id = send_data(to, channel, meshtastic_PortNum_TEXT_MESSAGE_APP, text, len, true, false);
    if (!id) return false;
    msg_t m;
    memset(&m, 0, sizeof(m));
    m.id = id;
    m.from = g_mesh.my_num;
    m.to = to;
    m.channel = channel;
    m.time = mesh_now();
    m.status = MSG_SENT;
    m.len = (uint8_t)len;
    memcpy(m.text, text, len);
    add_msg(&m);
    return true;
}

static bool send_admin(const meshtastic_AdminMessage *a) {
    if (!g_mesh.my_num) return false;
    return send_proto(g_mesh.my_num, 0, meshtastic_PortNum_ADMIN_APP, meshtastic_AdminMessage_fields, a, false, false) != 0;
}

static meshtastic_AdminMessage admin;

bool mesh_admin_set_owner(const char *long_name, const char *short_name) {
    const node_t *me = mesh_my_node();
    memset(&admin, 0, sizeof(admin));
    admin.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
    meshtastic_User *u = &admin.set_owner;
    if (me) {
        snprintf(u->id, sizeof(u->id), "%s", me->id);
        u->hw_model = (meshtastic_HardwareModel)me->hw_model;
        u->role = (meshtastic_Config_DeviceConfig_Role)me->role;
    }
    snprintf(u->long_name, sizeof(u->long_name), "%s", long_name);
    snprintf(u->short_name, sizeof(u->short_name), "%s", short_name);
    if (!send_admin(&admin)) return false;
    node_t *n = mesh_node_get(g_mesh.my_num);
    if (n) {
        snprintf(n->long_name, sizeof(n->long_name), "%s", long_name);
        snprintf(n->short_name, sizeof(n->short_name), "%s", short_name);
        n->has_user = true;
    }
    changed();
    return true;
}

bool mesh_admin_set_lora(const meshtastic_Config_LoRaConfig *lora) {
    memset(&admin, 0, sizeof(admin));
    admin.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
    admin.set_config.which_payload_variant = meshtastic_Config_lora_tag;
    admin.set_config.payload_variant.lora = *lora;
    if (!send_admin(&admin)) return false;
    g_mesh.lora = *lora;
    g_mesh.has_lora = true;
    changed();
    return true;
}

bool mesh_admin_set_device(const meshtastic_Config_DeviceConfig *dev) {
    memset(&admin, 0, sizeof(admin));
    admin.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
    admin.set_config.which_payload_variant = meshtastic_Config_device_tag;
    admin.set_config.payload_variant.device = *dev;
    if (!send_admin(&admin)) return false;
    g_mesh.device = *dev;
    g_mesh.has_device = true;
    changed();
    return true;
}

bool mesh_admin_reboot(int seconds) {
    memset(&admin, 0, sizeof(admin));
    admin.which_payload_variant = meshtastic_AdminMessage_reboot_seconds_tag;
    admin.reboot_seconds = seconds;
    return send_admin(&admin);
}

bool mesh_admin_set_favorite(uint32_t num, bool fav) {
    memset(&admin, 0, sizeof(admin));
    admin.which_payload_variant =
        fav ? meshtastic_AdminMessage_set_favorite_node_tag : meshtastic_AdminMessage_remove_favorite_node_tag;
    if (fav) admin.set_favorite_node = num;
    else admin.remove_favorite_node = num;
    if (!send_admin(&admin)) return false;
    node_t *n = mesh_node_find(num);
    if (n) n->is_favorite = fav;
    nodes_dirty = true;
    changed();
    return true;
}

bool mesh_admin_remove_node(uint32_t num) {
    if (num == g_mesh.my_num) return false;
    memset(&admin, 0, sizeof(admin));
    admin.which_payload_variant = meshtastic_AdminMessage_remove_by_nodenum_tag;
    admin.remove_by_nodenum = num;
    if (!send_admin(&admin)) return false;
    for (int i = 0; i < g_mesh.node_count; i++) {
        if (g_mesh.nodes[i].num == num) {
            g_mesh.nodes[i] = g_mesh.nodes[--g_mesh.node_count];
            break;
        }
    }
    nodes_dirty = true;
    changed();
    return true;
}

bool mesh_send_traceroute(uint32_t dest) {
    meshtastic_RouteDiscovery rd = meshtastic_RouteDiscovery_init_zero;
    const node_t *n = mesh_node_find(dest);
    uint32_t id = send_proto(dest, n ? n->channel : 0, meshtastic_PortNum_TRACEROUTE_APP, meshtastic_RouteDiscovery_fields,
                             &rd, true, true);
    if (!id) return false;
    g_mesh.tr_target = dest;
    g_mesh.tr_request_id = id;
    g_mesh.tr_pending = true;
    char name[40];
    mesh_node_longname(dest, name, sizeof(name));
    snprintf(g_mesh.tr_result, sizeof(g_mesh.tr_result), "Traceroute to %s sent, waiting...", name);
    changed();
    return true;
}

bool mesh_request_position(uint32_t dest) {
    meshtastic_Position pos = meshtastic_Position_init_zero;
    const node_t *me = mesh_my_node();
    if (me && me->has_pos) {
        pos.has_latitude_i = pos.has_longitude_i = true;
        pos.latitude_i = me->lat_i;
        pos.longitude_i = me->lon_i;
        pos.time = mesh_now();
    }
    const node_t *n = mesh_node_find(dest);
    return send_proto(dest, n ? n->channel : 0, meshtastic_PortNum_POSITION_APP, meshtastic_Position_fields, &pos, false,
                      true) != 0;
}

bool mesh_exchange_user_info(uint32_t dest) {
    const node_t *me = mesh_my_node();
    if (!me) return false;
    meshtastic_User u = meshtastic_User_init_zero;
    snprintf(u.id, sizeof(u.id), "%s", me->id);
    snprintf(u.long_name, sizeof(u.long_name), "%s", me->long_name);
    snprintf(u.short_name, sizeof(u.short_name), "%s", me->short_name);
    u.hw_model = (meshtastic_HardwareModel)me->hw_model;
    u.role = (meshtastic_Config_DeviceConfig_Role)me->role;
    const node_t *n = mesh_node_find(dest);
    return send_proto(dest, n ? n->channel : 0, meshtastic_PortNum_NODEINFO_APP, meshtastic_User_fields, &u, false, true) !=
           0;
}

// ------------------------------------------------------------------ receiving

static void format_route_hop(char *out, size_t n, uint32_t num, int8_t snr) {
    char name[8];
    mesh_node_label(num, name, sizeof(name));
    if (snr == INT8_MIN) snprintf(out, n, "%s (?dB)", name);
    else snprintf(out, n, "%s (%.1fdB)", name, snr / 4.0);
}

static void handle_traceroute(const meshtastic_MeshPacket *p, const meshtastic_RouteDiscovery *rd) {
    char *o = g_mesh.tr_result;
    size_t cap = sizeof(g_mesh.tr_result);
    size_t len = 0;
    char hop[32];
#define APPEND(...) do { if (len < cap) len += (size_t)snprintf(o + len, cap - len, __VA_ARGS__); } while (0)
    APPEND("Towards: ");
    mesh_node_label(g_mesh.my_num, hop, sizeof(hop));
    APPEND("%s", hop);
    for (int i = 0; i < rd->route_count; i++) {
        format_route_hop(hop, sizeof(hop), rd->route[i], i < rd->snr_towards_count ? rd->snr_towards[i] : INT8_MIN);
        APPEND(" > %s", hop);
    }
    format_route_hop(hop, sizeof(hop), p->from,
                     rd->snr_towards_count > rd->route_count ? rd->snr_towards[rd->route_count] : INT8_MIN);
    APPEND(" > %s", hop);
    if (rd->snr_back_count || rd->route_back_count) {
        APPEND("\nBack: ");
        mesh_node_label(p->from, hop, sizeof(hop));
        APPEND("%s", hop);
        for (int i = 0; i < rd->route_back_count; i++) {
            format_route_hop(hop, sizeof(hop), rd->route_back[i], i < rd->snr_back_count ? rd->snr_back[i] : INT8_MIN);
            APPEND(" > %s", hop);
        }
        format_route_hop(hop, sizeof(hop), g_mesh.my_num,
                         rd->snr_back_count > rd->route_back_count ? rd->snr_back[rd->route_back_count] : INT8_MIN);
        APPEND(" > %s", hop);
    }
#undef APPEND
    g_mesh.tr_pending = false;
    app_log("Traceroute: %s", g_mesh.tr_result);
}

static void handle_text(const meshtastic_MeshPacket *p) {
    const meshtastic_Data *d = &p->decoded;
    msg_t m;
    memset(&m, 0, sizeof(m));
    m.id = p->id;
    m.from = p->from;
    m.to = p->to;
    m.channel = p->channel;
    m.time = p->rx_time ? p->rx_time : mesh_now();
    m.rssi = (int16_t)p->rx_rssi;
    m.snr4 = (int8_t)(p->rx_snr * 4);
    m.hops = p->hop_start >= p->hop_limit ? (uint8_t)(p->hop_start - p->hop_limit) : 0;
    m.status = MSG_RECEIVED;
    size_t len = d->payload.size < sizeof(m.text) - 1 ? d->payload.size : sizeof(m.text) - 1;
    memcpy(m.text, d->payload.bytes, len);
    m.len = (uint8_t)len;

    // Drop duplicates (the radio can resend on reconnect)
    for (int i = msg_count - 1; i >= 0 && i >= msg_count - 50; i--) {
        if (msgs[i].id == m.id && msgs[i].from == m.from) return;
    }
    msg_t *stored = add_msg(&m);
    g_mesh.rx_text++;
    unread_add(mesh_msg_conv_key(stored));
    if (mesh_on_new_message) mesh_on_new_message(stored);
}

static void handle_routing(const meshtastic_MeshPacket *p, const meshtastic_Routing *r) {
    const meshtastic_Data *d = &p->decoded;
    if (r->which_variant != meshtastic_Routing_error_reason_tag || !d->request_id) return;
    meshtastic_Routing_Error err = r->error_reason;
    if (g_mesh.tr_pending && d->request_id == g_mesh.tr_request_id && err != meshtastic_Routing_Error_NONE) {
        g_mesh.tr_pending = false;
        snprintf(g_mesh.tr_result, sizeof(g_mesh.tr_result), "Traceroute failed: %s", mesh_routing_error_name(err));
        changed();
    }
    msg_t *m = find_my_msg(d->request_id);
    if (!m) return;
    if (err != meshtastic_Routing_Error_NONE) {
        set_msg_status(m, MSG_FAILED, (uint8_t)err);
    } else if (p->from == m->to || m->to == NODENUM_BROADCAST) {
        // Broadcasts only ever get implicit ACKs; treat them as delivered to the mesh.
        set_msg_status(m, m->to == NODENUM_BROADCAST ? MSG_ACKED : MSG_DELIVERED, 0);
    } else {
        set_msg_status(m, MSG_ACKED, 0);
    }
}

static void handle_packet(const meshtastic_MeshPacket *p) {
    g_mesh.rx_packets++;
    if (p->rx_time) set_clock(p->rx_time);

    node_t *n = mesh_node_get(p->from);
    if (n && p->from != g_mesh.my_num) {
        n->last_heard = p->rx_time ? p->rx_time : mesh_now();
        n->last_heard_up = mesh_uptime_s();
        if (p->rx_snr != 0) n->snr = p->rx_snr;
        if (p->hop_start) n->hops_away = (int8_t)(p->hop_start - p->hop_limit);
        n->via_mqtt = p->via_mqtt;
        if (p->to == NODENUM_BROADCAST && p->channel < MAX_CHANNELS) n->channel = p->channel;
        nodes_dirty = true;
    }
    if (p->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        changed();
        return;
    }
    const meshtastic_Data *d = &p->decoded;
    pb_istream_t is = pb_istream_from_buffer(d->payload.bytes, d->payload.size);

    switch (d->portnum) {
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
        handle_text(p);
        break;
    case meshtastic_PortNum_ROUTING_APP: {
        meshtastic_Routing r = meshtastic_Routing_init_zero;
        if (pb_decode(&is, meshtastic_Routing_fields, &r)) handle_routing(p, &r);
        break;
    }
    case meshtastic_PortNum_POSITION_APP: {
        meshtastic_Position pos = meshtastic_Position_init_zero;
        if (n && pb_decode(&is, meshtastic_Position_fields, &pos)) apply_position(n, &pos);
        break;
    }
    case meshtastic_PortNum_NODEINFO_APP: {
        meshtastic_User u = meshtastic_User_init_zero;
        if (n && pb_decode(&is, meshtastic_User_fields, &u)) apply_user(n, &u);
        break;
    }
    case meshtastic_PortNum_TELEMETRY_APP: {
        static meshtastic_Telemetry t;
        memset(&t, 0, sizeof(t));
        if (n && pb_decode(&is, meshtastic_Telemetry_fields, &t)) {
            if (t.which_variant == meshtastic_Telemetry_device_metrics_tag) {
                apply_metrics(n, &t.variant.device_metrics);
            } else if (t.which_variant == meshtastic_Telemetry_environment_metrics_tag) {
                const meshtastic_EnvironmentMetrics *e = &t.variant.environment_metrics;
                if (e->has_temperature) n->temperature = e->temperature;
                if (e->has_relative_humidity) n->humidity = e->relative_humidity;
                if (e->has_barometric_pressure) n->pressure = e->barometric_pressure;
                n->has_env = e->has_temperature || e->has_relative_humidity || e->has_barometric_pressure;
            }
        }
        break;
    }
    case meshtastic_PortNum_TRACEROUTE_APP: {
        meshtastic_RouteDiscovery rd = meshtastic_RouteDiscovery_init_zero;
        if (g_mesh.tr_pending && d->request_id == g_mesh.tr_request_id &&
            pb_decode(&is, meshtastic_RouteDiscovery_fields, &rd)) {
            handle_traceroute(p, &rd);
        }
        break;
    }
    default:
        break;
    }
    changed();
}

void mesh_handle_fromradio(const uint8_t *data, size_t len) {
    memset(&fr, 0, sizeof(fr));
    pb_istream_t is = pb_istream_from_buffer(data, len);
    if (!pb_decode(&is, meshtastic_FromRadio_fields, &fr)) {
        app_log("FromRadio decode error (%u bytes): %s", (unsigned)len, PB_GET_ERROR(&is));
        return;
    }
    switch (fr.which_payload_variant) {
    case meshtastic_FromRadio_packet_tag:
        handle_packet(&fr.packet);
        break;
    case meshtastic_FromRadio_my_info_tag:
        if (g_mesh.my_num && g_mesh.my_num != fr.my_info.my_node_num) {
            app_log("Different radio than last time, clearing node list");
            g_mesh.node_count = 0;
        }
        g_mesh.my_num = fr.my_info.my_node_num;
        g_mesh.reboot_count = (uint16_t)fr.my_info.reboot_count;
        snprintf(g_mesh.pio_env, sizeof(g_mesh.pio_env), "%s", fr.my_info.pio_env);
        app_log("My node !%08lx", (unsigned long)g_mesh.my_num);
        break;
    case meshtastic_FromRadio_node_info_tag:
        handle_node_info(&fr.node_info);
        break;
    case meshtastic_FromRadio_config_tag:
        switch (fr.config.which_payload_variant) {
        case meshtastic_Config_lora_tag:
            g_mesh.lora = fr.config.payload_variant.lora;
            g_mesh.has_lora = true;
            break;
        case meshtastic_Config_device_tag:
            g_mesh.device = fr.config.payload_variant.device;
            g_mesh.has_device = true;
            break;
        case meshtastic_Config_position_tag:
            g_mesh.position = fr.config.payload_variant.position;
            g_mesh.has_position = true;
            break;
        case meshtastic_Config_bluetooth_tag:
            g_mesh.bluetooth = fr.config.payload_variant.bluetooth;
            g_mesh.has_bluetooth = true;
            break;
        case meshtastic_Config_display_tag:
            g_mesh.display = fr.config.payload_variant.display;
            g_mesh.has_display = true;
            break;
        default:
            break;
        }
        break;
    case meshtastic_FromRadio_channel_tag:
        if (fr.channel.index >= 0 && fr.channel.index < MAX_CHANNELS) g_mesh.channels[fr.channel.index] = fr.channel;
        break;
    case meshtastic_FromRadio_config_complete_id_tag:
        if (fr.config_complete_id == g_mesh.config_nonce) {
            g_mesh.config_complete = true;
            app_log("Config complete: %d nodes", g_mesh.node_count);
            storage_nodes_save(g_mesh.nodes, g_mesh.node_count);
            nodes_dirty = false;
        }
        break;
    case meshtastic_FromRadio_queueStatus_tag:
        g_mesh.queue_free = fr.queueStatus.free;
        g_mesh.queue_max = fr.queueStatus.maxlen;
        if (fr.queueStatus.res != 0 && fr.queueStatus.mesh_packet_id) {
            msg_t *m = find_my_msg(fr.queueStatus.mesh_packet_id);
            if (m) set_msg_status(m, MSG_FAILED, 0);
        }
        break;
    case meshtastic_FromRadio_metadata_tag:
        g_mesh.metadata = fr.metadata;
        g_mesh.has_metadata = true;
        app_log("Radio firmware %s", fr.metadata.firmware_version);
        break;
    case meshtastic_FromRadio_clientNotification_tag:
        app_log("Radio: %s", fr.clientNotification.message);
        break;
    case meshtastic_FromRadio_rebooted_tag:
        app_log("Radio rebooted, reloading config");
        mesh_on_link_up();
        break;
    default:
        break;
    }
    changed();
}

bool mesh_wants_reads(void) { return g_mesh.link_up && !g_mesh.config_complete; }

void mesh_on_link_up(void) {
    g_mesh.link_up = true;
    g_mesh.config_complete = false;
    g_mesh.config_nonce = get_rand_32() | 1;
    memset(&tr, 0, sizeof(tr));
    tr.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    tr.want_config_id = g_mesh.config_nonce;
    send_toradio();
    ble_link_request_read();
    next_heartbeat = mesh_uptime_s() + 60;
    changed();
}

void mesh_on_link_down(void) {
    g_mesh.link_up = false;
    g_mesh.config_complete = false;
    g_mesh.tr_pending = false;
    // Anything still waiting will never get an answer on this link.
    for (int i = 0; i < msg_count; i++) {
        if (msgs[i].status == MSG_QUEUED) set_msg_status(&msgs[i], MSG_FAILED, 0);
    }
    changed();
}

void mesh_init(void) {
    memset(&g_mesh, 0, sizeof(g_mesh));
    for (int i = 0; i < MAX_CHANNELS; i++) g_mesh.channels[i].index = (int8_t)i;
    g_mesh.node_count = storage_nodes_load(g_mesh.nodes, MAX_NODES);
    for (int i = 0; i < g_mesh.node_count; i++) g_mesh.nodes[i].last_heard_up = 0;
    msg_count = storage_msg_load(msgs, MAX_MSGS);
    for (int i = 0; i < msg_count; i++) {
        if (msgs[i].seq >= next_seq) next_seq = msgs[i].seq + 1;
    }
    app_log("Loaded %d nodes, %d messages", g_mesh.node_count, msg_count);
}

void mesh_poll(void) {
    uint32_t now = mesh_uptime_s();
    if (nodes_dirty && g_mesh.config_complete && (int32_t)(now - nodes_save_at) >= 0) {
        nodes_save_at = now + 300;
        nodes_dirty = false;
        storage_nodes_save(g_mesh.nodes, g_mesh.node_count);
    }
    if (g_mesh.link_up && g_mesh.config_complete && (int32_t)(now - next_heartbeat) >= 0) {
        next_heartbeat = now + 300;
        memset(&tr, 0, sizeof(tr));
        tr.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
        tr.heartbeat.nonce = get_rand_32();
        send_toradio();
    }
}

// ------------------------------------------------------------------ names

const char *mesh_region_name(int r) {
    static const char *const names[] = {"UNSET",  "US",     "EU_433", "EU_868", "CN",     "JP",      "ANZ",
                                        "KR",     "TW",     "RU",     "IN",     "NZ_865", "TH",      "LORA_24",
                                        "UA_433", "UA_868", "MY_433", "MY_919", "SG_923", "PH_433",  "PH_868",
                                        "PH_915", "ANZ_433", "KZ_433", "KZ_863", "NP_865", "BR_902"};
    return r >= 0 && r < (int)(sizeof(names) / sizeof(names[0])) ? names[r] : "Other";
}

const char *mesh_preset_name(int p) {
    static const char *const names[] = {"LongFast",   "LongSlow",  "VeryLongSlow", "MediumSlow", "MediumFast",
                                        "ShortSlow",  "ShortFast", "LongModerate", "ShortTurbo", "LongTurbo",
                                        "LiteFast",   "LiteSlow",  "NarrowFast",   "NarrowSlow", "TinyFast",
                                        "TinySlow",   "MediumTurbo"};
    return p >= 0 && p < (int)(sizeof(names) / sizeof(names[0])) ? names[p] : "Custom";
}

const char *mesh_role_name(int r) {
    static const char *const names[] = {"CLIENT", "CLIENT_MUTE", "ROUTER",         "ROUTER_CLIENT", "REPEATER",
                                        "TRACKER", "SENSOR",     "TAK",            "CLIENT_HIDDEN", "LOST_AND_FOUND",
                                        "TAK_TRACKER", "ROUTER_LATE", "CLIENT_BASE"};
    return r >= 0 && r < (int)(sizeof(names) / sizeof(names[0])) ? names[r] : "?";
}

extern const char *const hw_model_names[];
extern const int hw_model_names_count;

const char *mesh_hw_name(int hw) {
    if (hw == 255) return "PRIVATE_HW";
    if (hw >= 0 && hw < hw_model_names_count && hw_model_names[hw][0]) return hw_model_names[hw];
    return "UNKNOWN";
}

const char *mesh_routing_error_name(int e) {
    switch (e) {
    case 0: return "OK";
    case 1: return "No route";
    case 2: return "Got NAK";
    case 3: return "Timeout";
    case 4: return "No interface";
    case 5: return "No ACK";
    case 6: return "No channel";
    case 7: return "Too large";
    case 8: return "No response";
    case 9: return "Duty cycle limit";
    case 32: return "Bad request";
    case 33: return "Not authorized";
    case 34: return "PKI failed";
    case 35: return "Unknown public key";
    case 36: return "Bad admin session";
    case 37: return "Admin key unauthorized";
    case 38: return "Rate limited";
    default: return "Error";
    }
}
