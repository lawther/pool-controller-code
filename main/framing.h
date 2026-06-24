#ifndef FRAMING_H
#define FRAMING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

typedef enum {
    FRAMING_NEED_MORE_DATA,        // incomplete, wait for more bytes
    FRAMING_NO_START_BYTE,         // no 0x02 found, buffer cleared
    FRAMING_BAD_HEADER_CHECKSUM,   // header checksum mismatch, resynced by 1
    FRAMING_BAD_CONTROL_BYTES,     // bytes 5-6 not 0x80 0x00, resynced by 1
    FRAMING_BAD_LENGTH,            // length field out of range, resynced by 1
    FRAMING_BAD_END_BYTE,          // end byte not 0x03, resynced by 1
    FRAMING_BAD_DATA_CHECKSUM,     // data checksum mismatch, resynced by 1
    FRAMING_FRAME_READY,           // valid frame written to out_frame
} framing_result_t;

// Context structure holding the reassembly buffer
typedef struct {
    uint8_t buffer[BUS_MESSAGE_MAX_SIZE];
    int len;
} framing_buffer_t;

/**
 * Initialize a framing buffer context.
 */
void framing_init(framing_buffer_t *fb);

/**
 * Add bytes to the framing buffer.
 * Returns true if bytes were successfully added, false if buffer is full.
 */
bool framing_add_bytes(framing_buffer_t *fb, const uint8_t *data, int len);

/**
 * Extract one complete message from the reassembly buffer.
 * On FRAMING_FRAME_READY the frame is written to out_frame and out_len; on
 * every other resync result (NO_START / BAD_*) the bytes that triggered the
 * resync are written there instead, so the caller can capture them.
 * FRAMING_NEED_MORE_DATA means stop and wait; all other results mean call
 * again immediately.
 */
framing_result_t framing_process_next(framing_buffer_t *fb, uint8_t *out_frame, int *out_len);

#endif // FRAMING_H
