#ifndef UNKNOWN_BUFFER_H
#define UNKNOWN_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "config.h"

#define UNKNOWN_BUFFER_CAPACITY         100
#define UNKNOWN_BUFFER_MAX_RAW_BYTES    64   // store at most the first 64 bytes of each frame

typedef struct {
    uint16_t raw_len;      // actual frame length (may exceed UNKNOWN_BUFFER_MAX_RAW_BYTES)
    uint8_t  raw[UNKNOWN_BUFFER_MAX_RAW_BYTES];  // first bytes of the frame
    uint32_t hit_count;
    time_t   first_seen;   // UTC epoch seconds (0 if NTP not synced)
    time_t   last_seen;    // UTC epoch seconds (0 if NTP not synced)
    bool     is_error;     // true = framing/checksum error; false = no handler matched
} unknown_entry_t;

void unknown_buffer_init(void);

// Record a frame. is_error=true for framing/checksum errors, false for unhandled frames.
void unknown_buffer_record(const uint8_t *data, int len, bool is_error);

void unknown_buffer_clear(void);

typedef struct {
    const unknown_entry_t *entries;
    int                   count;
} locked_unknown_buffer_t;

// Acquires the buffer mutex and returns a read-only pointer to the entry array.
// The caller MUST call unknown_buffer_unlock_after_read() when done.
locked_unknown_buffer_t unknown_buffer_lock_for_read(void);
void unknown_buffer_unlock_after_read(void);

#endif // UNKNOWN_BUFFER_H
