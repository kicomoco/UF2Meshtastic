// Wi-Fi station management on the CYW43 (shared with Bluetooth).
#include "net/wifi.h"

#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "log.h"
#include "storage.h"

#define CONNECT_TIMEOUT_MS 30000
#define RETRY_MIN_MS 10000
#define RETRY_MAX_MS 120000
#define MAX_APS 16

static wifi_state_t state = WIFI_OFF;
static bool sta_enabled;      // CYW43 station interface is up
static bool scan_forced_sta;  // station enabled only for a scan
static uint32_t auth_mode;
static int last_link;
static uint32_t attempt_start, retry_at, retry_delay;
static const char *status = "Off";
static char ip_text[16] = "-";
static uint32_t changes;

static wifi_ap_t aps[MAX_APS];
static int ap_count;
static bool scanning;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void set_state(wifi_state_t s, const char *text) {
    if (s != state || text != status) changes++;
    state = s;
    status = text;
}

static void sta_up(void) {
    if (sta_enabled) return;
    cyw43_arch_enable_sta_mode();
    sta_enabled = true;
}

static void sta_down(void) {
    if (!sta_enabled) return;
    cyw43_arch_disable_sta_mode();
    sta_enabled = false;
    snprintf(ip_text, sizeof(ip_text), "-");
}

static void start_connect(void) {
    sta_up();
    const char *pw = g_settings.wifi_pass[0] ? g_settings.wifi_pass : NULL;
    uint32_t auth = pw ? auth_mode : CYW43_AUTH_OPEN;
    int r = cyw43_arch_wifi_connect_async(g_settings.wifi_ssid, pw, auth);
    attempt_start = now_ms();
    last_link = CYW43_LINK_JOIN;
    if (r) {
        app_log("Wi-Fi: connect error %d", r);
        set_state(WIFI_FAILED, "Connect error");
        retry_at = now_ms() + retry_delay;
        return;
    }
    app_log("Wi-Fi: connecting to %s", g_settings.wifi_ssid);
    set_state(WIFI_CONNECTING, "Connecting...");
}

static void schedule_retry(const char *why) {
    app_log("Wi-Fi: %s, retry in %lus", why, (unsigned long)(retry_delay / 1000));
    set_state(WIFI_FAILED, why);
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    retry_at = now_ms() + retry_delay;
    if (retry_delay < RETRY_MAX_MS) retry_delay *= 2;
}

void wifi_apply_settings(void) {
    retry_delay = RETRY_MIN_MS;
    auth_mode = CYW43_AUTH_WPA2_AES_PSK;
    if (!g_settings.wifi_enabled) {
        if (sta_enabled) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        if (sta_enabled && scanning) {
            scan_forced_sta = true; // turned off when the scan finishes
        } else if (sta_enabled) {
            sta_down();
        }
        set_state(WIFI_OFF, "Off");
        return;
    }
    scan_forced_sta = false;
    if (!g_settings.wifi_ssid[0]) {
        sta_up();
        set_state(WIFI_NO_NETWORK, "No network chosen");
        return;
    }
    if (sta_enabled) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    start_connect();
}

void wifi_init(void) {
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], CYW43_HOST_NAME);
    wifi_apply_settings();
}

void wifi_poll(void) {
    if (scanning && !cyw43_wifi_scan_active(&cyw43_state)) {
        scanning = false;
        changes++;
        app_log("Wi-Fi: scan found %d networks", ap_count);
        if (scan_forced_sta && !g_settings.wifi_enabled) sta_down();
        scan_forced_sta = false;
    }
    if (!g_settings.wifi_enabled || !g_settings.wifi_ssid[0]) return;

    uint32_t t = now_ms();
    int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    switch (state) {
    case WIFI_CONNECTING:
        if (link == CYW43_LINK_UP) {
            const ip4_addr_t *ip = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
            snprintf(ip_text, sizeof(ip_text), "%s", ip4addr_ntoa(ip));
            app_log("Wi-Fi: connected, IP %s", ip_text);
            retry_delay = RETRY_MIN_MS;
            set_state(WIFI_CONNECTED, "Connected");
        } else if (link == CYW43_LINK_BADAUTH) {
            // Try WPA3/WPA2 transition mode once before giving up on the password.
            if (auth_mode == CYW43_AUTH_WPA2_AES_PSK) {
                auth_mode = CYW43_AUTH_WPA3_WPA2_AES_PSK;
                cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
                start_connect();
            } else {
                auth_mode = CYW43_AUTH_WPA2_AES_PSK;
                schedule_retry("Wrong password");
            }
        } else if (link == CYW43_LINK_NONET) {
            schedule_retry("Network not found");
        } else if (link == CYW43_LINK_FAIL) {
            schedule_retry("Connection failed");
        } else if (t - attempt_start > CONNECT_TIMEOUT_MS) {
            schedule_retry(link == CYW43_LINK_NOIP ? "No IP address (DHCP)" : "Timed out");
        } else if (link != last_link) {
            last_link = link;
            if (link == CYW43_LINK_NOIP) set_state(WIFI_CONNECTING, "Getting IP address...");
        }
        break;
    case WIFI_CONNECTED:
        if (link != CYW43_LINK_UP) {
            app_log("Wi-Fi: link lost (%d)", link);
            snprintf(ip_text, sizeof(ip_text), "-");
            retry_delay = RETRY_MIN_MS;
            set_state(WIFI_FAILED, "Link lost");
            retry_at = t + 2000;
        }
        break;
    case WIFI_FAILED:
        if ((int32_t)(t - retry_at) >= 0 && !scanning) start_connect();
        break;
    default:
        break;
    }
}

wifi_state_t wifi_state(void) { return state; }
bool wifi_online(void) { return state == WIFI_CONNECTED; }
const char *wifi_status_text(void) { return status; }
const char *wifi_ip_text(void) { return ip_text; }
uint32_t wifi_change_counter(void) { return changes; }

int wifi_rssi(void) {
    int32_t rssi = 0;
    if (state != WIFI_CONNECTED || cyw43_wifi_get_rssi(&cyw43_state, &rssi)) return 0;
    return (int)rssi;
}

static int scan_cb(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (!r || r->ssid_len == 0 || r->ssid_len > 32) return 0;
    char ssid[33];
    memcpy(ssid, r->ssid, r->ssid_len);
    ssid[r->ssid_len] = 0;
    for (int i = 0; i < ap_count; i++) {
        if (strcmp(aps[i].ssid, ssid) == 0) {
            if (r->rssi > aps[i].rssi) aps[i].rssi = r->rssi;
            return 0;
        }
    }
    // Keep the strongest MAX_APS networks, sorted by signal.
    int pos = ap_count;
    while (pos > 0 && aps[pos - 1].rssi < r->rssi) pos--;
    if (pos >= MAX_APS) return 0;
    int last = ap_count < MAX_APS ? ap_count : MAX_APS - 1;
    memmove(&aps[pos + 1], &aps[pos], sizeof(wifi_ap_t) * (size_t)(last - pos));
    memcpy(aps[pos].ssid, ssid, sizeof(ssid));
    aps[pos].rssi = r->rssi;
    aps[pos].secure = r->auth_mode != 0;
    if (ap_count < MAX_APS) ap_count++;
    return 0;
}

bool wifi_scan_start(void) {
    if (scanning) return true;
    if (!sta_enabled) {
        sta_up();
        scan_forced_sta = true;
    }
    cyw43_wifi_scan_options_t opts = {0};
    ap_count = 0;
    if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_cb) != 0) {
        app_log("Wi-Fi: scan failed to start");
        return false;
    }
    scanning = true;
    changes++;
    return true;
}

bool wifi_scan_active(void) { return scanning; }
int wifi_scan_count(void) { return ap_count; }
const wifi_ap_t *wifi_scan_result(int i) { return i >= 0 && i < ap_count ? &aps[i] : NULL; }
