#ifndef CHANNEL_POWER_H
#define CHANNEL_POWER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "pool_state.h"

// Load the per-channel configured wattage cache from NVS. Call once at boot.
void channel_power_init(void);

// Configured wattage for a channel (1-based). 0 means unset — either never
// configured, or deliberately cleared because the channel's device reports
// its own real power (see channel_power_get_effective).
uint16_t channel_power_get_configured(uint8_t channel_id);

// Persist a channel's configured wattage to NVS and update the cache.
esp_err_t channel_power_set_configured(uint8_t channel_id, uint16_t watts);

// Resolve the live power draw for a channel (1-based):
//   1. Real device telemetry, when available, always wins (currently the
//      Filter channel's Viron XT variable-speed pump reading).
//   2. Otherwise the manually configured wattage, gated by active state.
//   3. If neither applies (no telemetry and configured == 0), there is
//      nothing to report — returns false.
bool channel_power_get_effective(const pool_state_t *state, uint8_t channel_id, uint16_t *out_watts);

#endif // CHANNEL_POWER_H
