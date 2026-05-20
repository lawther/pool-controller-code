/**
 * Mock esp_log.h for host-based testing.
 *
 * Logs route through log_capture_emit(). When log_capture_enabled is true
 * (set by the replay harness) emissions are buffered for comparison; otherwise
 * they print to stdout, matching the original unit-test behaviour.
 */

#ifndef ESP_LOG_H
#define ESP_LOG_H

#include <stdio.h>

#include "log_capture.h"

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;

#define ESP_LOGE(tag, format, ...) log_capture_emit('E', tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) log_capture_emit('W', tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) log_capture_emit('I', tag, format, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) log_capture_emit('D', tag, format, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) log_capture_emit('V', tag, format, ##__VA_ARGS__)

#endif
