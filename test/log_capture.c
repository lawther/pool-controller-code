#include "log_capture.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

char   log_capture_buf[LOG_CAPTURE_BUF_SIZE];
size_t log_capture_len = 0;
bool   log_capture_enabled = false;

void log_capture_reset(void)
{
    log_capture_len = 0;
    log_capture_buf[0] = '\0';
}

void log_capture_emit(char level, const char *tag, const char *fmt, ...)
{
    char line[2048];
    int n = snprintf(line, sizeof line, "%c %s: ", level, tag);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;

    int total = n + m;
    if (total >= (int)sizeof line) total = (int)sizeof line - 1;
    line[total] = '\0';

    if (log_capture_enabled) {
        if (log_capture_len + (size_t)total + 2 < LOG_CAPTURE_BUF_SIZE) {
            memcpy(log_capture_buf + log_capture_len, line, (size_t)total);
            log_capture_len += (size_t)total;
            log_capture_buf[log_capture_len++] = '\n';
            log_capture_buf[log_capture_len] = '\0';
        }
    } else {
        printf("%s\n", line);
    }
}
