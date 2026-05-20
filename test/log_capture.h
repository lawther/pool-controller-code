#ifndef LOG_CAPTURE_H
#define LOG_CAPTURE_H

#include <stddef.h>
#include <stdbool.h>

#define LOG_CAPTURE_BUF_SIZE 65536

extern char   log_capture_buf[LOG_CAPTURE_BUF_SIZE];
extern size_t log_capture_len;
extern bool   log_capture_enabled;

void log_capture_reset(void);
void log_capture_emit(char level, const char *tag, const char *fmt, ...);

#endif
