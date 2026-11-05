#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

#define LOG_LINES 160
#define LOG_LINE_LEN 80

void app_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int app_log_count(void);
// 0 = oldest retained line
const char *app_log_line(int idx);
uint32_t app_log_serial(void);

#endif
