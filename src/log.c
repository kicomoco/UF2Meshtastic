#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include "pico/time.h"

static char lines[LOG_LINES][LOG_LINE_LEN];
static int head, count;
static uint32_t serial;

void app_log(const char *fmt, ...) {
    uint32_t s = to_ms_since_boot(get_absolute_time()) / 1000;
    char *line = lines[head];
    int n = snprintf(line, LOG_LINE_LEN, "%02lu:%02lu:%02lu ", (unsigned long)(s / 3600),
                     (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, LOG_LINE_LEN - n, fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    head = (head + 1) % LOG_LINES;
    if (count < LOG_LINES) count++;
    serial++;
}

int app_log_count(void) { return count; }

const char *app_log_line(int idx) {
    if (idx < 0 || idx >= count) return "";
    int start = (head - count + LOG_LINES) % LOG_LINES;
    return lines[(start + idx) % LOG_LINES];
}

uint32_t app_log_serial(void) { return serial; }
