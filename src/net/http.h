#ifndef NET_HTTP_H
#define NET_HTTP_H

#include <stdbool.h>

enum {
    HTTP_ERR_DNS = -1,
    HTTP_ERR_CONNECT = -2,
    HTTP_ERR_TIMEOUT = -3,
    HTTP_ERR_PROTOCOL = -4,
    HTTP_ERR_FILE = -5,
    HTTP_ERR_MEMORY = -6,
    HTTP_ERR_CLOSED = -7,
};

// result: HTTP status code (the file is only kept for 200) or HTTP_ERR_*.
typedef void (*http_done_cb)(void *ctx, int result);

// Downloads https?://host/path into `dest` on the SD card (written to a
// temporary file and renamed when complete). One request at a time; the
// connection is kept open for a following request to the same host.
bool http_get_file(const char *host, bool tls, const char *path, const char *dest, http_done_cb cb, void *ctx);
bool http_busy(void);
// True while a connection (busy or idle keep-alive) is open.
bool http_connected(void);
// Closes an idle keep-alive connection (frees the TLS buffers).
void http_close_idle(void);
void http_poll(void);
const char *http_error_text(int result);

#endif
