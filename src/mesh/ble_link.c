// BLE central link to a Meshtastic radio (Heltec V3 etc.).
//
// Protocol (see meshtastic.org/docs/development/device/client-api):
//  * Service 6ba1b218-15a8-461f-9fa8-5dcae273eafd
//  * ToRadio   (write)        - encoded ToRadio protobufs
//  * FromRadio (read)         - one encoded FromRadio per read, empty when drained
//  * FromNum   (read/notify)  - notifies when new FromRadio data is available
// The radio requires an encrypted, MITM-authenticated link: it shows a PIN on
// its screen that the user enters here (passkey entry, we are keyboard-only).
#include "mesh/ble_link.h"

#include <string.h>
#include <stdio.h>
#include "btstack.h"
#include "pico/stdlib.h"

#include "mesh/mesh.h"
#include "storage.h"
#include "log.h"

static const uint8_t UUID_SERVICE[16] = {0x6b, 0xa1, 0xb2, 0x18, 0x15, 0xa8, 0x46, 0x1f,
                                         0x9f, 0xa8, 0x5d, 0xca, 0xe2, 0x73, 0xea, 0xfd};
static const uint8_t UUID_TORADIO[16] = {0xf7, 0x5c, 0x76, 0xd2, 0x12, 0x9e, 0x4d, 0xad,
                                         0xa1, 0xdd, 0x78, 0x66, 0x12, 0x44, 0x01, 0xe7};
static const uint8_t UUID_FROMRADIO[16] = {0x2c, 0x55, 0xe6, 0x9e, 0x49, 0x93, 0x11, 0xed,
                                           0xb8, 0x78, 0x02, 0x42, 0xac, 0x12, 0x00, 0x02};
static const uint8_t UUID_FROMNUM[16] = {0xed, 0x9d, 0xa1, 0x8c, 0xa8, 0x00, 0x4f, 0x66,
                                         0xa6, 0x70, 0xaa, 0x75, 0x47, 0xe3, 0x44, 0x53};

#define CONNECT_TIMEOUT_MS 12000
#define PAIRING_TIMEOUT_MS 15000
#define PASSKEY_TIMEOUT_MS 120000
#define USER_SCAN_MS 30000
#define RECONNECT_SCAN_MS 8000
#define PERIODIC_READ_MS 15000

typedef enum { PH_NONE, PH_SERVICE, PH_CHARS, PH_CCCD, PH_WRITE, PH_READ } gatt_phase_t;

static link_state_t state = LINK_OFF;
static hci_con_handle_t con = HCI_CON_HANDLE_INVALID;
static uint8_t peer_addr[6];
static uint8_t peer_type;
static char peer_name[32];
static char last_error[64];
static bool user_disconnect;
static bool need_passkey;
static uint16_t mtu = 23;

static gatt_client_service_t service;
static bool service_found;
static gatt_client_characteristic_t ch_to, ch_from, ch_num;
static bool has_to, has_from, has_num;
static gatt_client_notification_t notif;
static bool notif_registered;

static gatt_phase_t phase = PH_NONE;
static bool read_requested;
static uint16_t last_read_len;

#define TXQ_LEN 10
static struct {
    uint16_t len;
    uint8_t data[520];
} txq[TXQ_LEN];
static int txq_head, txq_tail;

static scan_result_t scan[SCAN_MAX];
static int scan_count;
static bool scanning;
static bool reconnect_scan;
static uint32_t scan_deadline;

static bool want_reconnect;
static int reconnect_attempt;
static uint32_t reconnect_at;
static uint32_t state_deadline;
static uint32_t next_periodic_read;

static btstack_packet_callback_registration_t hci_cb, sm_cb;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static bool deadline_passed(uint32_t t) { return (int32_t)(now_ms() - t) >= 0; }

static void set_error(const char *msg) {
    snprintf(last_error, sizeof(last_error), "%s", msg);
    app_log("BLE: %s", msg);
}

static const char *addr_str(const uint8_t *a) { return bd_addr_to_str(a); }

static bool uuid_eq(const uint8_t *a, const uint8_t *be) {
    if (memcmp(a, be, 16) == 0) return true;
    for (int i = 0; i < 16; i++) {
        if (a[i] != be[15 - i]) return false;
    }
    return true;
}

static void gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// ------------------------------------------------------------------ helpers

static void schedule_reconnect(uint32_t delay_ms) {
    if (!g_settings.auto_connect || !g_settings.has_last_device) {
        want_reconnect = false;
        return;
    }
    want_reconnect = true;
    reconnect_at = now_ms() + delay_ms;
}

static void reset_connection_state(void) {
    if (notif_registered) {
        gatt_client_stop_listening_for_characteristic_value_updates(&notif);
        notif_registered = false;
    }
    con = HCI_CON_HANDLE_INVALID;
    phase = PH_NONE;
    read_requested = false;
    need_passkey = false;
    has_to = has_from = has_num = service_found = false;
    txq_head = txq_tail = 0;
    mtu = 23;
}

static void stop_scan_internal(void) {
    if (scanning) {
        gap_stop_scan();
        scanning = false;
    }
    reconnect_scan = false;
    if (state == LINK_SCANNING) state = LINK_IDLE;
}

static void start_scan_internal(uint32_t duration_ms, bool for_reconnect) {
    if (state != LINK_IDLE && state != LINK_SCANNING) return;
    gap_set_scan_parameters(1, 0x0030, 0x0030);
    if (!scanning) gap_start_scan();
    scanning = true;
    reconnect_scan = for_reconnect;
    scan_deadline = now_ms() + duration_ms;
    state = LINK_SCANNING;
}

static void do_connect(const uint8_t addr[6], uint8_t type) {
    stop_scan_internal();
    memcpy(peer_addr, addr, 6);
    peer_type = type;
    last_error[0] = 0;
    // 15-30 ms connection interval, 4 s supervision timeout.
    gap_set_connection_parameters(0x0060, 0x0030, 12, 24, 0, 400, 0, 0);
    uint8_t st = gap_connect(peer_addr, (bd_addr_type_t)peer_type);
    if (st != ERROR_CODE_SUCCESS) {
        char buf[48];
        snprintf(buf, sizeof(buf), "connect failed (0x%02x)", st);
        set_error(buf);
        state = LINK_IDLE;
        return;
    }
    app_log("BLE: connecting to %s (%s)", peer_name[0] ? peer_name : "?", addr_str(peer_addr));
    state = LINK_CONNECTING;
    state_deadline = now_ms() + CONNECT_TIMEOUT_MS;
}

static void connect_failed(void) {
    state = LINK_IDLE;
    reconnect_attempt++;
    uint32_t delay = 3000u * (uint32_t)(reconnect_attempt < 10 ? reconnect_attempt : 10);
    schedule_reconnect(delay);
}

// ------------------------------------------------------------------ discovery

static void start_discovery(void) {
    state = LINK_DISCOVERING;
    phase = PH_SERVICE;
    service_found = false;
    app_log("BLE: discovering Meshtastic service");
    gatt_client_discover_primary_services_by_uuid128(gatt_handler, con, UUID_SERVICE);
}

static void link_ready(void) {
    gatt_client_get_mtu(con, &mtu);
    state = LINK_CONNECTED;
    phase = PH_NONE;
    reconnect_attempt = 0;
    last_error[0] = 0;
    app_log("BLE: connected, MTU %u", mtu);
    if (mtu < 512) app_log("BLE: MTU below packet size, using long reads/writes");

    g_settings.has_last_device = true;
    memcpy(g_settings.last_addr, peer_addr, 6);
    g_settings.last_addr_type = peer_type;
    snprintf(g_settings.last_name, sizeof(g_settings.last_name), "%s", peer_name);
    settings_save();

    next_periodic_read = now_ms() + PERIODIC_READ_MS;
    mesh_on_link_up();
}

// ------------------------------------------------------------------ GATT op pump

// FromRadio reassembly for long (blob) reads when the MTU is below the packet size.
static uint8_t rx_buf[520];
static uint16_t rx_len;
static bool rx_overflow;

static void pump(void) {
    if (state != LINK_CONNECTED || phase != PH_NONE || con == HCI_CON_HANDLE_INVALID) return;
    if (txq_head != txq_tail) {
        uint16_t len = txq[txq_tail].len;
        uint8_t st = len <= mtu - 3
                         ? gatt_client_write_value_of_characteristic(gatt_handler, con, ch_to.value_handle, len,
                                                                     txq[txq_tail].data)
                         : gatt_client_write_long_value_of_characteristic(gatt_handler, con, ch_to.value_handle, len,
                                                                          txq[txq_tail].data);
        if (st == ERROR_CODE_SUCCESS) {
            phase = PH_WRITE;
        } else if (st != GATT_CLIENT_BUSY) {
            app_log("BLE: ToRadio write rejected (0x%02x, %u bytes)", st, len);
            txq_tail = (txq_tail + 1) % TXQ_LEN;
        }
        return;
    }
    if (read_requested) {
        // The radio pops one packet per read request; blob continuation reads
        // return the rest of the same packet, so a long read is safe.
        uint8_t st = gatt_client_read_long_value_of_characteristic_using_value_handle(gatt_handler, con,
                                                                                     ch_from.value_handle);
        if (st == ERROR_CODE_SUCCESS) {
            phase = PH_READ;
            last_read_len = 0;
            rx_len = 0;
            rx_overflow = false;
        }
    }
}

static void gatt_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t ev = hci_event_packet_get_type(packet);

    switch (ev) {
    case GATT_EVENT_MTU:
        mtu = gatt_event_mtu_get_MTU(packet);
        break;

    case GATT_EVENT_SERVICE_QUERY_RESULT:
        gatt_event_service_query_result_get_service(packet, &service);
        service_found = true;
        break;

    case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
        gatt_client_characteristic_t c;
        gatt_event_characteristic_query_result_get_characteristic(packet, &c);
        if (uuid_eq(c.uuid128, UUID_TORADIO)) {
            ch_to = c;
            has_to = true;
        } else if (uuid_eq(c.uuid128, UUID_FROMRADIO)) {
            ch_from = c;
            has_from = true;
        } else if (uuid_eq(c.uuid128, UUID_FROMNUM)) {
            ch_num = c;
            has_num = true;
        }
        break;
    }

    case GATT_EVENT_LONG_CHARACTERISTIC_VALUE_QUERY_RESULT:
        if (phase == PH_READ &&
            gatt_event_long_characteristic_value_query_result_get_value_handle(packet) == ch_from.value_handle) {
            uint16_t off = gatt_event_long_characteristic_value_query_result_get_value_offset(packet);
            uint16_t n = gatt_event_long_characteristic_value_query_result_get_value_length(packet);
            if (off + n > sizeof(rx_buf)) {
                rx_overflow = true;
            } else {
                memcpy(rx_buf + off, gatt_event_long_characteristic_value_query_result_get_value(packet), n);
                if (off + n > rx_len) rx_len = off + n;
            }
        }
        break;

    case GATT_EVENT_NOTIFICATION:
        if (has_num && gatt_event_notification_get_value_handle(packet) == ch_num.value_handle) {
            read_requested = true;
        }
        break;

    case GATT_EVENT_QUERY_COMPLETE: {
        uint8_t att = gatt_event_query_complete_get_att_status(packet);
        gatt_phase_t done = phase;
        phase = PH_NONE;
        switch (done) {
        case PH_SERVICE:
            if (att != ATT_ERROR_SUCCESS || !service_found) {
                set_error("Meshtastic service not found");
                gap_disconnect(con);
                return;
            }
            phase = PH_CHARS;
            gatt_client_discover_characteristics_for_service(gatt_handler, con, &service);
            return;
        case PH_CHARS:
            if (att != ATT_ERROR_SUCCESS || !has_to || !has_from) {
                set_error("Meshtastic characteristics missing");
                gap_disconnect(con);
                return;
            }
            if (has_num) {
                gatt_client_listen_for_characteristic_value_updates(&notif, gatt_handler, con, &ch_num);
                notif_registered = true;
                phase = PH_CCCD;
                gatt_client_write_client_characteristic_configuration(
                    gatt_handler, con, &ch_num, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                return;
            }
            link_ready();
            break;
        case PH_CCCD:
            if (att != ATT_ERROR_SUCCESS) app_log("BLE: FromNum notify failed (0x%02x), polling", att);
            link_ready();
            break;
        case PH_WRITE:
            if (att != ATT_ERROR_SUCCESS) app_log("BLE: ToRadio write error 0x%02x", att);
            if (txq_head != txq_tail) txq_tail = (txq_tail + 1) % TXQ_LEN;
            read_requested = true;
            break;
        case PH_READ:
            if (att != ATT_ERROR_SUCCESS) {
                app_log("BLE: FromRadio read error 0x%02x", att);
                read_requested = false;
            } else {
                last_read_len = rx_len;
                if (rx_overflow) app_log("BLE: oversized FromRadio packet dropped");
                else if (rx_len) mesh_handle_fromradio(rx_buf, rx_len);
                read_requested = last_read_len > 0 || mesh_wants_reads();
            }
            break;
        default:
            break;
        }
        pump();
        break;
    }
    default:
        break;
    }
}

// ------------------------------------------------------------------ HCI / SM events

static void parse_adv(const uint8_t *data, uint8_t len, char *name, size_t name_len, bool *meshtastic) {
    ad_context_t it;
    for (ad_iterator_init(&it, len, data); ad_iterator_has_more(&it); ad_iterator_next(&it)) {
        uint8_t type = ad_iterator_get_data_type(&it);
        uint8_t dlen = ad_iterator_get_data_len(&it);
        const uint8_t *d = ad_iterator_get_data(&it);
        if (type == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME || type == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) {
            size_t n = dlen < name_len - 1 ? dlen : name_len - 1;
            memcpy(name, d, n);
            name[n] = 0;
        } else if (type == BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS ||
                   type == BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS) {
            for (int i = 0; i + 16 <= dlen; i += 16) {
                if (uuid_eq(d + i, UUID_SERVICE)) *meshtastic = true;
            }
        }
    }
}

static void handle_adv(const uint8_t *packet) {
    bd_addr_t addr;
    gap_event_advertising_report_get_address(packet, addr);
    uint8_t type = gap_event_advertising_report_get_address_type(packet);
    int8_t rssi = (int8_t)gap_event_advertising_report_get_rssi(packet);
    char name[32] = "";
    bool mesh = false;
    parse_adv(gap_event_advertising_report_get_data(packet), gap_event_advertising_report_get_data_length(packet),
              name, sizeof(name), &mesh);

    scan_result_t *r = NULL;
    for (int i = 0; i < scan_count; i++) {
        if (memcmp(scan[i].addr, addr, 6) == 0) {
            r = &scan[i];
            break;
        }
    }
    if (!r) {
        if (scan_count < SCAN_MAX) {
            r = &scan[scan_count++];
        } else {
            // Replace the weakest non-Meshtastic entry
            int worst = -1;
            for (int i = 0; i < scan_count; i++) {
                if (!scan[i].meshtastic && (worst < 0 || scan[i].rssi < scan[worst].rssi)) worst = i;
            }
            if (worst < 0 || !mesh) return;
            r = &scan[worst];
        }
        memset(r, 0, sizeof(*r));
        memcpy(r->addr, addr, 6);
    }
    r->addr_type = type;
    r->rssi = rssi;
    bool was_mesh = r->meshtastic, had_name = r->name[0] != 0;
    r->meshtastic |= mesh;
    if (name[0]) snprintf(r->name, sizeof(r->name), "%s", name);
    if (r->meshtastic && (!was_mesh || (!had_name && r->name[0])))
        app_log("BLE: found %s %s (%d dBm)", r->name[0] ? r->name : "?", addr_str(addr), rssi);
    r->last_seen_ms = now_ms();

    if (reconnect_scan && g_settings.has_last_device) {
        bool match = memcmp(addr, g_settings.last_addr, 6) == 0 ||
                     (r->meshtastic && r->name[0] && strcmp(r->name, g_settings.last_name) == 0);
        if (match) {
            app_log("BLE: found %s, reconnecting", r->name[0] ? r->name : addr_str(addr));
            snprintf(peer_name, sizeof(peer_name), "%s", r->name[0] ? r->name : g_settings.last_name);
            do_connect(r->addr, r->addr_type);
        }
    }
}

static void on_connection_complete(uint8_t status, hci_con_handle_t handle, uint8_t role) {
    if (state != LINK_CONNECTING) return;
    if (status != ERROR_CODE_SUCCESS) {
        char buf[48];
        snprintf(buf, sizeof(buf), "connection failed (0x%02x)", status);
        set_error(buf);
        connect_failed();
        return;
    }
    if (role != HCI_ROLE_MASTER) return;
    con = handle;
    state = LINK_PAIRING;
    state_deadline = now_ms() + PAIRING_TIMEOUT_MS;
    app_log("BLE: link up, securing connection");
    sm_request_pairing(con);
}

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            bd_addr_t local;
            gap_local_bd_addr(local);
            app_log("BLE: ready, local address %s", addr_str(local));
            state = LINK_IDLE;
            if (g_settings.has_last_device) {
                snprintf(peer_name, sizeof(peer_name), "%s", g_settings.last_name);
                schedule_reconnect(200);
            }
        } else if (btstack_event_state_get_state(packet) == HCI_STATE_OFF) {
            state = LINK_OFF;
        }
        break;

    case GAP_EVENT_ADVERTISING_REPORT:
        if (scanning) handle_adv(packet);
        break;

    case HCI_EVENT_LE_META:
        if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
            on_connection_complete(hci_subevent_le_connection_complete_get_status(packet),
                                   hci_subevent_le_connection_complete_get_connection_handle(packet),
                                   hci_subevent_le_connection_complete_get_role(packet));
        }
        break;

    case HCI_EVENT_DISCONNECTION_COMPLETE: {
        if (hci_event_disconnection_complete_get_connection_handle(packet) != con) break;
        uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
        app_log("BLE: disconnected (reason 0x%02x)", reason);
        bool was_connected = state == LINK_CONNECTED;
        reset_connection_state();
        state = LINK_IDLE;
        mesh_on_link_down();
        if (user_disconnect) {
            user_disconnect = false;
            want_reconnect = false;
        } else if (was_connected) {
            reconnect_attempt = 0;
            schedule_reconnect(3000);
        } else {
            connect_failed();
        }
        break;
    }
    default:
        break;
    }
}

static void sm_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
    case SM_EVENT_JUST_WORKS_REQUEST:
        sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
        break;
    case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
        sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
        break;
    case SM_EVENT_PASSKEY_INPUT_NUMBER:
        app_log("BLE: enter the PIN shown on the radio");
        need_passkey = true;
        state_deadline = now_ms() + PASSKEY_TIMEOUT_MS;
        break;
    case SM_EVENT_PAIRING_COMPLETE: {
        bool was_waiting_for_pin = need_passkey;
        need_passkey = false;
        if (state != LINK_PAIRING) break;
        uint8_t status = sm_event_pairing_complete_get_status(packet);
        uint8_t reason = sm_event_pairing_complete_get_reason(packet);
        if (status == ERROR_CODE_SUCCESS) {
            app_log("BLE: paired and bonded");
            start_discovery();
        } else if (reason == SM_REASON_PAIRING_NOT_SUPPORTED) {
            app_log("BLE: radio has pairing disabled, continuing unencrypted");
            start_discovery();
        } else if (reason == SM_REASON_PASSKEY_ENTRY_FAILED || reason == SM_REASON_CONFIRM_VALUE_FAILED ||
                   reason == SM_REASON_DHKEY_CHECK_FAILED) {
            set_error("Pairing failed - wrong PIN?");
            user_disconnect = true;
            gap_disconnect(con);
        } else if (was_waiting_for_pin) {
            // Radio gave up while we waited for the PIN; don't loop prompting.
            set_error("PIN not entered in time - press Enter to retry");
            user_disconnect = true;
            gap_disconnect(con);
        } else {
            char buf[48];
            snprintf(buf, sizeof(buf), "pairing failed (st 0x%02x r 0x%02x)", status, reason);
            set_error(buf);
            gap_disconnect(con);
        }
        break;
    }
    case SM_EVENT_REENCRYPTION_COMPLETE: {
        if (state != LINK_PAIRING) break;
        uint8_t status = sm_event_reencryption_complete_get_status(packet);
        if (status == ERROR_CODE_SUCCESS) {
            app_log("BLE: encrypted with stored bond");
            start_discovery();
        } else if (status == ERROR_CODE_PIN_OR_KEY_MISSING || status == ERROR_CODE_AUTHENTICATION_FAILURE) {
            // The radio has forgotten us (e.g. it was reset): pair again.
            app_log("BLE: stored bond rejected (0x%02x), re-pairing", status);
            gap_delete_bonding((bd_addr_type_t)peer_type, peer_addr);
            state_deadline = now_ms() + PAIRING_TIMEOUT_MS;
            sm_request_pairing(con);
        } else {
            // Link-level failure (e.g. radio dropped a stale connection): keep the bond and retry.
            app_log("BLE: encryption failed (0x%02x), will retry", status);
            gap_disconnect(con);
        }
        break;
    }
    default:
        break;
    }
}

// ------------------------------------------------------------------ public API

void ble_link_init(void) {
    l2cap_init();
    sm_init();
    gatt_client_init();

    sm_set_io_capabilities(IO_CAPABILITY_KEYBOARD_ONLY);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_MITM_PROTECTION | SM_AUTHREQ_BONDING);

    hci_cb.callback = hci_handler;
    hci_add_event_handler(&hci_cb);
    sm_cb.callback = sm_handler;
    sm_add_event_handler(&sm_cb);

    hci_power_control(HCI_POWER_ON);
}

void ble_link_poll(void) {
    switch (state) {
    case LINK_SCANNING:
        if (deadline_passed(scan_deadline)) {
            bool was_reconnect = reconnect_scan;
            stop_scan_internal();
            if (was_reconnect) connect_failed();
        }
        break;
    case LINK_CONNECTING:
        if (deadline_passed(state_deadline)) {
            gap_connect_cancel();
            set_error("connection timed out");
            connect_failed();
        }
        break;
    case LINK_PAIRING:
        if (deadline_passed(state_deadline)) {
            if (need_passkey) {
                set_error("PIN entry timed out");
                need_passkey = false;
                user_disconnect = true;
                gap_disconnect(con);
            } else {
                app_log("BLE: no security response, trying without");
                start_discovery();
            }
        }
        break;
    case LINK_IDLE:
        if (want_reconnect && deadline_passed(reconnect_at)) {
            want_reconnect = false;
            if (reconnect_attempt % 2 == 0) {
                snprintf(peer_name, sizeof(peer_name), "%s", g_settings.last_name);
                do_connect(g_settings.last_addr, g_settings.last_addr_type);
            } else {
                app_log("BLE: scanning for %s", g_settings.last_name[0] ? g_settings.last_name : "last radio");
                start_scan_internal(RECONNECT_SCAN_MS, true);
            }
        }
        break;
    case LINK_CONNECTED:
        if (deadline_passed(next_periodic_read)) {
            next_periodic_read = now_ms() + PERIODIC_READ_MS;
            read_requested = true;
        }
        pump();
        break;
    default:
        break;
    }
}

link_state_t ble_link_state(void) { return state; }

const char *ble_link_state_name(void) {
    switch (state) {
    case LINK_OFF: return "Bluetooth off";
    case LINK_IDLE: return want_reconnect ? "Waiting to reconnect" : "Disconnected";
    case LINK_SCANNING: return reconnect_scan ? "Searching for radio" : "Scanning";
    case LINK_CONNECTING: return "Connecting";
    case LINK_PAIRING: return need_passkey ? "Waiting for PIN" : "Securing link";
    case LINK_DISCOVERING: return "Discovering";
    case LINK_CONNECTED: return "Connected";
    }
    return "?";
}

const char *ble_link_last_error(void) { return last_error; }
const char *ble_link_peer_name(void) { return peer_name; }
const uint8_t *ble_link_peer_addr(void) { return peer_addr; }
uint16_t ble_link_mtu(void) { return mtu; }

void ble_link_start_scan(void) {
    if (state == LINK_IDLE || state == LINK_SCANNING) {
        want_reconnect = false;
        // Drop stale entries
        int j = 0;
        for (int i = 0; i < scan_count; i++) {
            if (now_ms() - scan[i].last_seen_ms < 60000) scan[j++] = scan[i];
        }
        scan_count = j;
        start_scan_internal(USER_SCAN_MS, false);
    }
}

void ble_link_stop_scan(void) { stop_scan_internal(); }
bool ble_link_scanning(void) { return scanning; }
int ble_link_scan_count(void) { return scan_count; }
const scan_result_t *ble_link_scan_result(int idx) { return idx >= 0 && idx < scan_count ? &scan[idx] : NULL; }

void ble_link_connect(const uint8_t addr[6], uint8_t addr_type, const char *name) {
    if (state == LINK_SCANNING) stop_scan_internal();
    if (state != LINK_IDLE) return;
    want_reconnect = false;
    reconnect_attempt = 0;
    snprintf(peer_name, sizeof(peer_name), "%s", name ? name : "");
    do_connect(addr, addr_type);
}

void ble_link_disconnect(void) {
    want_reconnect = false;
    switch (state) {
    case LINK_SCANNING:
        stop_scan_internal();
        break;
    case LINK_CONNECTING:
        gap_connect_cancel();
        state = LINK_IDLE;
        break;
    case LINK_PAIRING:
    case LINK_DISCOVERING:
    case LINK_CONNECTED:
        user_disconnect = true;
        gap_disconnect(con);
        break;
    default:
        break;
    }
}

void ble_link_forget_bonds(void) {
    for (int i = 0; i < le_device_db_max_count(); i++) {
        int type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t a;
        sm_key_t irk;
        le_device_db_info(i, &type, a, irk);
        if (type != BD_ADDR_TYPE_UNKNOWN) le_device_db_remove(i);
    }
    g_settings.has_last_device = false;
    g_settings.last_name[0] = 0;
    settings_save();
    want_reconnect = false;
    app_log("BLE: all pairings forgotten");
}

bool ble_link_needs_passkey(void) { return need_passkey && state == LINK_PAIRING; }

void ble_link_enter_passkey(uint32_t passkey) {
    if (!ble_link_needs_passkey()) return;
    need_passkey = false;
    state_deadline = now_ms() + PAIRING_TIMEOUT_MS;
    sm_passkey_input(con, passkey);
}

void ble_link_cancel_passkey(void) {
    if (!ble_link_needs_passkey()) return;
    need_passkey = false;
    set_error("Pairing cancelled");
    user_disconnect = true;
    gap_disconnect(con);
}

bool ble_link_send(const uint8_t *data, uint16_t len) {
    if (state != LINK_CONNECTED || len > sizeof(txq[0].data)) return false;
    int next = (txq_head + 1) % TXQ_LEN;
    if (next == txq_tail) return false;
    memcpy(txq[txq_head].data, data, len);
    txq[txq_head].len = len;
    txq_head = next;
    pump();
    return true;
}

void ble_link_request_read(void) {
    read_requested = true;
    pump();
}
