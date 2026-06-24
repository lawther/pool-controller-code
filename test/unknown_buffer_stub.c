#include <stdint.h>
#include "../main/unknown_buffer.h"

// No-op stub — message_decoder.c calls this but the real implementation
// uses FreeRTOS primitives not available in the host test environment.
void unknown_buffer_record(const uint8_t *data, int len, unknown_reason_t reason)
{
    (void)data; (void)len; (void)reason;
}
