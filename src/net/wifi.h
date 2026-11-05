#ifndef NET_WIFI_H
#define NET_WIFI_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    WIFI_OFF,        // disabled in settings
    WIFI_NO_NETWORK, // enabled but no network configured
    WIFI_CONNECTING,
    WIFI_CONNECTED,  // associated and has an IP address
    WIFI_FAILED,     // last attempt failed; retried automatically
} wifi_state_t;

typedef struct {
    char ssid[33];
    int16_t rssi;
    bool secure;
} wifi_ap_t;

// Call once after cyw43_arch_init(); applies g_settings.
void wifi_init(void);
void wifi_poll(void);
// Re-apply g_settings (enable/disable, new credentials).
void wifi_apply_settings(void);

wifi_state_t wifi_state(void);
bool wifi_online(void);
// Short human-readable status, e.g. "Connected", "Wrong password".
const char *wifi_status_text(void);
// Dotted IP address, or "-".
const char *wifi_ip_text(void);
int wifi_rssi(void);
// Increments whenever anything shown in the UI changes.
uint32_t wifi_change_counter(void);

// Network scan (turns the radio on temporarily if Wi-Fi is disabled).
bool wifi_scan_start(void);
bool wifi_scan_active(void);
int wifi_scan_count(void);
const wifi_ap_t *wifi_scan_result(int i);

#endif
