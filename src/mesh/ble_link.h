#ifndef MESH_BLE_LINK_H
#define MESH_BLE_LINK_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    LINK_OFF = 0,     // Bluetooth not powered yet
    LINK_IDLE,        // ready, not connected
    LINK_SCANNING,
    LINK_CONNECTING,
    LINK_PAIRING,     // securing the link (may wait for passkey)
    LINK_DISCOVERING, // GATT service discovery
    LINK_CONNECTED,   // Meshtastic service ready
} link_state_t;

#define SCAN_MAX 16

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    bool meshtastic;
    char name[32];
    uint32_t last_seen_ms;
} scan_result_t;

void ble_link_init(void);
void ble_link_poll(void);

link_state_t ble_link_state(void);
const char *ble_link_state_name(void);
const char *ble_link_last_error(void);
const char *ble_link_peer_name(void);
const uint8_t *ble_link_peer_addr(void);
uint16_t ble_link_mtu(void);

void ble_link_start_scan(void);
void ble_link_stop_scan(void);
bool ble_link_scanning(void);
int ble_link_scan_count(void);
const scan_result_t *ble_link_scan_result(int idx);

void ble_link_connect(const uint8_t addr[6], uint8_t addr_type, const char *name);
void ble_link_disconnect(void);
void ble_link_forget_bonds(void);

// Pairing passkey entry (the Meshtastic radio shows the PIN on its screen)
bool ble_link_needs_passkey(void);
void ble_link_enter_passkey(uint32_t passkey);
void ble_link_cancel_passkey(void);

// Queue an encoded ToRadio protobuf for transmission.
bool ble_link_send(const uint8_t *data, uint16_t len);
// Ask the link to read FromRadio until it is empty.
void ble_link_request_read(void);

#endif
