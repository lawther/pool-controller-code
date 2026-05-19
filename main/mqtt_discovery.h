#ifndef MQTT_DISCOVERY_H
#define MQTT_DISCOVERY_H

#include "pool_state.h"

// Publish all Home Assistant discovery messages
// This should be called once when MQTT connects
void mqtt_publish_discovery(void);

// Publish individual channel discovery (called when channel first configured)
void mqtt_publish_channel_discovery_single(int channel_num, const char *channel_name);

// Publish individual light discovery (called when light first configured or name changes)
void mqtt_publish_light_discovery_single(int zone_num, const char *zone_name);

// Publish individual valve discovery (called when valve first configured or name changes)
void mqtt_publish_valve_discovery_single(int valve_num, const char *valve_name);

// Publish individual heater discovery (called when heater first publishes state)
void mqtt_publish_heater_discovery_single(int index);

// Publish individual temperature-sensor discovery for a (source, sensor) pair.
// Called the first time `mqtt_publish_temperature_reading` fires for each
// (dev_idx, sensor_index). `single_sensor_source` selects the HA entity name
// and state topic shape — see mqtt_publish.c.
void mqtt_publish_temperature_sensor_discovery_single(
    uint8_t addr_hi, uint8_t addr_lo,
    uint8_t sensor_index, bool single_sensor_source);

// Publish favourite select discovery (called on connect and when names/enable flags change)
void mqtt_publish_favourite_discovery_single(const pool_state_t *state);

#endif // MQTT_DISCOVERY_H
