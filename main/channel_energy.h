#ifndef CHANNEL_ENERGY_H
#define CHANNEL_ENERGY_H

// Starts a low-priority background task that periodically integrates each
// channel's effective power (channel_power_get_effective) over time into a
// cumulative kWh total and publishes it as an HA energy sensor.
//
// The accumulator is RAM-only — it resets to 0 on reboot rather than being
// checkpointed to NVS. That's a deliberate trade-off to avoid flash wear from
// frequent writes; HA's state_class: total_increasing already treats a value
// drop as a meter reset without corrupting long-term statistics, so this is
// safe to leave unpersisted.
void channel_energy_start(void);

#endif // CHANNEL_ENERGY_H
