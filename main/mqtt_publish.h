#ifndef MQTT_PUBLISH_H
#define MQTT_PUBLISH_H

#include <stdint.h>
#include <stdbool.h>
#include "pool_state.h"

// Publish pool/spa setpoints and temperature scale to `pool/<id>/setpoints/state`.
void mqtt_publish_setpoints(const pool_state_t *current_state);

// Publish a single temperature reading from a device's sensor.
// `dev_idx` indexes into `pool_state.seen_devices[]`; `sensor_index` is 1 or 2.
// Topic: `pool/<id>/temperature/<slug>/<index>/state` (multi-sensor),
//        `pool/<id>/temperature/<slug>/state`         (single-sensor; index ignored).
void mqtt_publish_temperature_reading(const pool_state_t *current_state, int dev_idx, uint8_t sensor_index);

// Publish heater state from pool state (index 0-based)
void mqtt_publish_heater(const pool_state_t *current_state, int index);

// Publish mode (Pool/Spa) from pool state
void mqtt_publish_mode(const pool_state_t *current_state);

// Publish channel state by ID (1-8) from pool state
void mqtt_publish_channel(const pool_state_t *current_state, uint8_t channel_id);

// Publish light zone state (1-4) from pool state
void mqtt_publish_light(const pool_state_t *current_state, uint8_t zone);

// Publish chlorinator data (pH and ORP) from pool state
void mqtt_publish_chlorinator(const pool_state_t *current_state);

// Publish valve state by number (1-based) from pool state
void mqtt_publish_valve(const pool_state_t *current_state, uint8_t valve_num);

// Publish favourite/mode select state; re-triggers discovery if options changed
void mqtt_publish_favourite(const pool_state_t *state);

#endif // MQTT_PUBLISH_H
