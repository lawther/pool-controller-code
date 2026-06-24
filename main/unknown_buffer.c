#include "unknown_buffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>

#define MUTEX_TIMEOUT_MS 100

static unknown_entry_t s_entries[UNKNOWN_BUFFER_CAPACITY];
static int              s_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

void unknown_buffer_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_count = 0;
}

// Adds `data` to the record of unknown data frames.
//
// If the data matches an existing record, the hit count and last seen time are updated.
// Otherwise, a new record is created, evicting the least frequently used record if the
// buffer is full.
//
// Note on matching: A match is defined as the same error status, the same length, and
// the same first `UNKNOWN_BUFFER_MAX_RAW_BYTES` bytes. As we cap stored bytes to
// `UNKNOWN_BUFFER_MAX_RAW_BYTES`, this means we can't distinguish between two different
// frames that only differ in the bytes after `UNKNOWN_BUFFER_MAX_RAW_BYTES`. This is a
// known limitation of this implementation.
void unknown_buffer_record(const uint8_t *data, int len, bool is_error)
{
    if (!s_mutex || !data || len <= 0) return;

    int store_len = (len < UNKNOWN_BUFFER_MAX_RAW_BYTES) ? len : UNKNOWN_BUFFER_MAX_RAW_BYTES;

    time_t now = time(NULL);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return;

    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].is_error == is_error &&
            s_entries[i].raw_len == (uint16_t)len &&
            memcmp(s_entries[i].raw, data, (size_t)store_len) == 0) {
            s_entries[i].hit_count++;
            s_entries[i].last_seen = now;
            xSemaphoreGive(s_mutex);
            return;
        }
    }

    int slot;
    if (s_count < UNKNOWN_BUFFER_CAPACITY) {
        slot = s_count++;
    } else {
        // LFU eviction: evict the entry with the fewest hits; break ties on oldest first_seen
        slot = 0;
        for (int i = 1; i < UNKNOWN_BUFFER_CAPACITY; i++) {
            if (s_entries[i].hit_count < s_entries[slot].hit_count ||
                (s_entries[i].hit_count == s_entries[slot].hit_count &&
                 s_entries[i].first_seen < s_entries[slot].first_seen)) {
                slot = i;
            }
        }
    }

    unknown_entry_t *e = &s_entries[slot];
    e->raw_len   = (uint16_t)len;
    memcpy(e->raw, data, (size_t)store_len);
    e->hit_count  = 1;
    e->first_seen = now;
    e->last_seen  = now;
    e->is_error   = is_error;

    xSemaphoreGive(s_mutex);
}

void unknown_buffer_clear(void)
{
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        s_count = 0;
        xSemaphoreGive(s_mutex);
    }
}

locked_unknown_buffer_t unknown_buffer_lock_for_read(void)
{
    locked_unknown_buffer_t locked_buf = {
        .entries = NULL,
        .count = 0
    };

    if (!s_mutex) {
        return locked_buf;
    }
    
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return locked_buf;
    }
    
    locked_buf.entries = s_entries;
    locked_buf.count = s_count;
    return locked_buf;
}

void unknown_buffer_unlock_after_read(void)
{
    if (s_mutex) xSemaphoreGive(s_mutex);
}
