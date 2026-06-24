#include <stdint.h>
#include <stdbool.h>

// No-op stub — message_decoder.c calls this but the real implementation
// uses FreeRTOS primitives not available in the host test environment.
void unknown_buffer_record(const uint8_t *data, int len, bool is_error)
{
    (void)data; (void)len; (void)is_error;
}
