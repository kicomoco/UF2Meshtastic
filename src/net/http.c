// Minimal HTTP/1.1 GET client (lwIP altcp, optional TLS) that streams the
// response body to a file on the SD card.
#include "net/http.h"

#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "mbedtls/ssl.h"
#include "mbedtls/platform_time.h"
#include "ff.h"

#include "log.h"

#define TIMEOUT_MS 20000
#define IDLE_CLOSE_MS 10000

typedef enum {
    ST_IDLE,       // no connection
    ST_READY,      // connected, no request in flight (keep-alive)
    ST_RESOLVING,
    ST_CONNECTING,
    ST_HEADERS,
    ST_BODY,       // Content-Length delimited
    ST_BODY_CLOSE, // delimited by the server closing the connection
    ST_CHUNK_SIZE,
    ST_CHUNK_DATA,
    ST_CHUNK_CRLF,
    ST_TRAILER,
} http_state_t;

static http_state_t st = ST_IDLE;
static struct altcp_pcb *pcb;
static struct altcp_tls_config *tls_conf;
static uint32_t generation; // invalidates late DNS callbacks

// Current connection
static char conn_host[64];
static bool conn_tls;
static bool conn_reused;

// Current request
static char req_path[96];
static char dest_path[96];
static char tmp_path[100];
static http_done_cb done_cb;
static void *done_ctx;
static bool req_active;
static bool got_bytes;
static int retries;

// Response
static int status_code;
static long remaining;
static bool keep_alive;
static bool status_line_seen;
static char line[160];
static int line_len;
static FIL file;
static bool file_open;
static bool file_error;
static uint32_t last_activity;
// Set when a connection is aborted; lwIP callbacks must then return ERR_ABRT.
static bool aborted;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)(time_us_64() / 1000); }

static void start_request(void);

const char *http_error_text(int r) {
    switch (r) {
    case HTTP_ERR_DNS: return "DNS lookup failed";
    case HTTP_ERR_CONNECT: return "Connection failed";
    case HTTP_ERR_TIMEOUT: return "Timed out";
    case HTTP_ERR_PROTOCOL: return "Bad response";
    case HTTP_ERR_FILE: return "SD card write failed";
    case HTTP_ERR_MEMORY: return "Out of memory";
    case HTTP_ERR_CLOSED: return "Connection closed";
    default: return "HTTP error";
    }
}

static void drop_pcb(bool abort) {
    if (!pcb) return;
    altcp_arg(pcb, NULL);
    altcp_recv(pcb, NULL);
    altcp_err(pcb, NULL);
    if (abort || altcp_close(pcb) != ERR_OK) {
        altcp_abort(pcb);
        aborted = true;
    }
    pcb = NULL;
}

static void close_file(bool keep) {
    if (!file_open) return;
    f_close(&file);
    file_open = false;
    if (keep && !file_error) {
        f_unlink(dest_path);
        if (f_rename(tmp_path, dest_path) != FR_OK) {
            f_unlink(tmp_path);
            file_error = true;
        }
    } else {
        f_unlink(tmp_path);
    }
}

static void finish(int result) {
    if (!req_active) return;
    bool ok = result == 200;
    close_file(ok);
    if (ok && file_error) result = HTTP_ERR_FILE;
    req_active = false;
    struct mallinfo mi = mallinfo();
    app_log("HTTP: %s -> %d (heap %d used, %d peak)", req_path, result, mi.uordblks, mi.arena);
    http_done_cb cb = done_cb;
    done_cb = NULL;
    if (cb) cb(done_ctx, result);
}

// Connection-level failure. A request that failed on a reused keep-alive
// connection before any response arrived is retried on a fresh connection.
static void fail(int err) {
    bool retry = req_active && conn_reused && !got_bytes && retries == 0;
    drop_pcb(true);
    st = ST_IDLE;
    generation++;
    if (retry) {
        retries++;
        start_request();
        return;
    }
    finish(err);
}

// ------------------------------------------------------------ response parsing

static void header_line(void) {
    line[line_len] = 0;
    if (line_len && line[line_len - 1] == '\r') line[--line_len] = 0;
    if (!status_line_seen) {
        status_line_seen = true;
        const char *sp = strchr(line, ' ');
        status_code = sp ? atoi(sp + 1) : 0;
        if (strncmp(line, "HTTP/1.0", 8) == 0) keep_alive = false;
        return;
    }
    if (line_len == 0) {
        // End of headers
        if (remaining == -2) {
            st = ST_CHUNK_SIZE;
        } else if (remaining >= 0) {
            st = ST_BODY;
        } else {
            st = ST_BODY_CLOSE;
            keep_alive = false;
        }
        return;
    }
    char *colon = strchr(line, ':');
    if (!colon) return;
    *colon = 0;
    char *v = colon + 1;
    while (*v == ' ') v++;
    for (char *c = line; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (char *c = v; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (strcmp(line, "content-length") == 0 && remaining != -2) remaining = atol(v);
    else if (strcmp(line, "transfer-encoding") == 0 && strstr(v, "chunked")) remaining = -2;
    else if (strcmp(line, "connection") == 0 && strstr(v, "close")) keep_alive = false;
}

static void body_data(const uint8_t *p, size_t n) {
    if (status_code != 200 || !file_open || file_error || !n) return;
    UINT wr = 0;
    if (f_write(&file, p, (UINT)n, &wr) != FR_OK || wr != n) file_error = true;
}

static void response_complete(void) {
    if (!keep_alive) {
        drop_pcb(false);
        st = ST_IDLE;
    } else {
        st = ST_READY;
    }
    finish(status_code);
}

// Returns false once the response is complete (remaining bytes are ignored).
static bool feed(const uint8_t *p, size_t n) {
    while (n) {
        switch (st) {
        case ST_HEADERS:
        case ST_CHUNK_SIZE:
        case ST_TRAILER: {
            uint8_t c = *p++;
            n--;
            if (c != '\n') {
                if (line_len < (int)sizeof(line) - 1) line[line_len++] = (char)c;
                break;
            }
            if (st == ST_HEADERS) {
                header_line();
                if (st == ST_BODY && remaining == 0) {
                    response_complete();
                    return false;
                }
            } else if (st == ST_CHUNK_SIZE) {
                line[line_len] = 0;
                remaining = strtol(line, NULL, 16);
                st = remaining > 0 ? ST_CHUNK_DATA : ST_TRAILER;
            } else {
                // Trailer section ends with an empty line
                if (line_len == 0 || (line_len == 1 && line[0] == '\r')) {
                    response_complete();
                    return false;
                }
            }
            line_len = 0;
            break;
        }
        case ST_BODY:
        case ST_CHUNK_DATA: {
            size_t take = n < (size_t)remaining ? n : (size_t)remaining;
            body_data(p, take);
            p += take;
            n -= take;
            remaining -= (long)take;
            if (remaining == 0) {
                if (st == ST_BODY) {
                    response_complete();
                    return false;
                }
                st = ST_CHUNK_CRLF;
                remaining = 2;
            }
            break;
        }
        case ST_CHUNK_CRLF:
            p++;
            n--;
            if (--remaining == 0) {
                st = ST_CHUNK_SIZE;
                line_len = 0;
            }
            break;
        case ST_BODY_CLOSE:
            body_data(p, n);
            n = 0;
            break;
        default:
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------ lwIP callbacks

static err_t recv_cb(void *arg, struct altcp_pcb *conn, struct pbuf *p, err_t err) {
    (void)arg;
    if (conn != pcb) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    aborted = false;
    if (!p || err != ERR_OK) {
        if (p) pbuf_free(p);
        // Server closed the connection
        if (st == ST_BODY_CLOSE) {
            drop_pcb(false);
            st = ST_IDLE;
            finish(status_code);
        } else if (st == ST_READY) {
            drop_pcb(false);
            st = ST_IDLE;
        } else {
            fail(HTTP_ERR_CLOSED);
        }
        return aborted ? ERR_ABRT : ERR_OK;
    }
    last_activity = now_ms();
    if (st != ST_READY && st != ST_IDLE) {
        got_bytes = true;
        for (struct pbuf *q = p; q; q = q->next) {
            if (!feed((const uint8_t *)q->payload, q->len)) break;
        }
    }
    if (pcb == conn) altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return aborted ? ERR_ABRT : ERR_OK;
}

static void err_cb(void *arg, err_t err) {
    (void)arg;
    app_log("HTTP: connection error %d", err);
    pcb = NULL; // already freed by lwIP
    if (req_active) fail(st == ST_CONNECTING ? HTTP_ERR_CONNECT : HTTP_ERR_CLOSED);
    else st = ST_IDLE;
}

static void send_request(void) {
    char req[320];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: PicoCalc-Meshtastic/" APP_VERSION " (+https://github.com/kicomoco/UF2Meshtastic)\r\n"
                     "Accept: image/png\r\n"
                     "Connection: keep-alive\r\n\r\n",
                     req_path, conn_host);
    status_code = 0;
    status_line_seen = false;
    remaining = -1;
    keep_alive = true;
    line_len = 0;
    got_bytes = false;
    st = ST_HEADERS;
    last_activity = now_ms();
    if (altcp_write(pcb, req, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK || altcp_output(pcb) != ERR_OK) {
        fail(HTTP_ERR_CONNECT);
    }
}

static err_t connected_cb(void *arg, struct altcp_pcb *conn, err_t err) {
    (void)arg;
    if (conn != pcb) return ERR_OK;
    aborted = false;
    if (err != ERR_OK) fail(HTTP_ERR_CONNECT);
    else send_request();
    return aborted ? ERR_ABRT : ERR_OK;
}

static void connect_to(const ip_addr_t *addr) {
    if (conn_tls) {
        if (!tls_conf) tls_conf = altcp_tls_create_config_client(NULL, 0);
        if (!tls_conf) {
            fail(HTTP_ERR_MEMORY);
            return;
        }
        pcb = altcp_tls_new(tls_conf, IPADDR_TYPE_V4);
        if (pcb) mbedtls_ssl_set_hostname((mbedtls_ssl_context *)altcp_tls_context(pcb), conn_host);
    } else {
        pcb = altcp_tcp_new_ip_type(IPADDR_TYPE_V4);
    }
    if (!pcb) {
        fail(HTTP_ERR_MEMORY);
        return;
    }
    altcp_recv(pcb, recv_cb);
    altcp_err(pcb, err_cb);
    st = ST_CONNECTING;
    last_activity = now_ms();
    if (altcp_connect(pcb, addr, conn_tls ? 443 : 80, connected_cb) != ERR_OK) fail(HTTP_ERR_CONNECT);
}

static void dns_cb(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name;
    if ((uint32_t)(uintptr_t)arg != generation || st != ST_RESOLVING) return;
    if (!addr) {
        fail(HTTP_ERR_DNS);
        return;
    }
    connect_to(addr);
}

static void start_request(void) {
    conn_reused = false;
    st = ST_RESOLVING;
    last_activity = now_ms();
    ip_addr_t addr;
    err_t e = dns_gethostbyname(conn_host, &addr, dns_cb, (void *)(uintptr_t)generation);
    if (e == ERR_OK) connect_to(&addr);
    else if (e != ERR_INPROGRESS) fail(HTTP_ERR_DNS);
}

// ------------------------------------------------------------ public API

bool http_get_file(const char *host, bool tls, const char *path, const char *dest, http_done_cb cb, void *ctx) {
    if (req_active) return false;
    snprintf(req_path, sizeof(req_path), "%s", path);
    snprintf(dest_path, sizeof(dest_path), "%s", dest);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dest);
    file_error = false;
    if (f_open(&file, tmp_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    file_open = true;
    done_cb = cb;
    done_ctx = ctx;
    req_active = true;
    retries = 0;

    if (st == ST_READY && pcb && conn_tls == tls && strcmp(conn_host, host) == 0) {
        conn_reused = true;
        send_request();
        return true;
    }
    if (st != ST_IDLE) {
        drop_pcb(false);
        st = ST_IDLE;
    }
    generation++;
    snprintf(conn_host, sizeof(conn_host), "%s", host);
    conn_tls = tls;
    start_request();
    return true;
}

bool http_busy(void) { return req_active; }
bool http_connected(void) { return pcb != NULL; }

void http_close_idle(void) {
    if (st == ST_READY) {
        drop_pcb(false);
        st = ST_IDLE;
    }
}

void http_poll(void) {
    uint32_t t = now_ms();
    if (req_active && t - last_activity > TIMEOUT_MS) {
        app_log("HTTP: timeout (state %d)", (int)st);
        retries = 1; // no transparent retry after a timeout
        fail(HTTP_ERR_TIMEOUT);
    } else if (st == ST_READY && t - last_activity > IDLE_CLOSE_MS) {
        http_close_idle();
    }
}
