#ifndef BUS_H
#define BUS_H

#include <stdint.h>
#include <stddef.h>

// Initialise the bus UART (GPIO, driver, TX inversion).
void bus_init(void);

// Read bytes from the bus UART, waiting up to timeout_ms for the first byte.
// Returns as soon as any bytes are available (frames may arrive split across
// calls), or 0 on timeout. Our own transmissions are echoed back here too.
int bus_read(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);

// Parse a hex-encoded message and send it to the bus UART.
// hex_string: space-separated or packed hex bytes,
//   e.g. "02 00 F0 FF FF 80 00 39 0E B7 08 04 0C 03"
// Returns the number of bytes sent, or -1 on parse or send error.
int bus_send_message(const char *hex_string);

// Send a raw byte buffer to the bus UART.
// Waits for the bus to be quiet before transmitting, then checks the echo of
// the frame; if another device talked over it, backs off and retries (up to
// BUS_TX_MAX_ATTEMPTS). Logs the TX bytes and flashes the TX LED on success.
// Returns the number of bytes sent, or -1 on error or persistent collision.
int bus_send_bytes(const uint8_t *data, size_t len);

#endif // BUS_H
