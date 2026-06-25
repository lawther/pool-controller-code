#include "heap_monitor.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "HEAP_MON";

// How often to log heap stats. Long enough that the overhead (one log line) is
// negligible, short enough to give useful resolution on a multi-day downward
// trend.
#define HEAP_MONITOR_INTERVAL_MS  (5 * 60 * 1000)

static void heap_monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        // free_now    — heap available right now
        // free_min    — lowest free heap seen since boot (leak/pressure watermark)
        // largest     — largest single allocatable block; a gap between this and
        //               free_now that widens over time indicates fragmentation
        uint32_t free_now = esp_get_free_heap_size();
        uint32_t free_min = esp_get_minimum_free_heap_size();
        size_t   largest  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

        ESP_LOGI(TAG, "heap: free=%lu B, min_free=%lu B, largest_block=%u B",
                 (unsigned long)free_now, (unsigned long)free_min, (unsigned)largest);

        vTaskDelay(pdMS_TO_TICKS(HEAP_MONITOR_INTERVAL_MS));
    }
}

void heap_monitor_start(void)
{
    // Low priority (1) and a small stack: this task is never time-critical and
    // only ever formats one log line. The stack still needs room for the
    // logging path (vprintf plus the TCP log-forward hook's 256-byte buffer).
    xTaskCreate(heap_monitor_task, "heap_mon", 3072, NULL, 1, NULL);
}
