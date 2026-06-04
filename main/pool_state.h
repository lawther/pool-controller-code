#ifndef POOL_STATE_H
#define POOL_STATE_H

#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Channel states
#define CHANNEL_STATE_COUNT 6
extern const char *CHANNEL_STATE_NAMES[];

// Lighting states
#define LIGHTING_STATE_COUNT 3
extern const char *LIGHTING_STATE_NAMES[];

// Lighting colors
#define LIGHTING_COLOR_COUNT 51
extern const char *LIGHTING_COLOR_NAMES[];

// Lighting zone preset names
#define LIGHT_ZONE_NAME_COUNT 6
extern const char *LIGHT_ZONE_NAME_TABLE[];

// Struct definitions
typedef struct {
    uint8_t id;
    char name[32];
    uint8_t type;
    uint8_t state;
    bool active;       // true if currently running (e.g. turned on by timer)
    bool configured;
} channel_state_t;

typedef struct {
    uint8_t zone;
    uint8_t state;
    uint8_t color;
    uint8_t name_id;    // Preset name code (0x00=Pool, 0x01=Spa, etc.)
    bool active;
    bool multicolor;          // true if zone supports colour-changing (0xA0+ Slot 0x01 = 0x01)
    bool multicolor_valid;    // true once a 0xA0+ multicolor message has been received
    bool name_valid;          // true once a 0xB0+ name message has been received
    bool configured;
} lighting_state_t;

typedef struct {
    char name[32];      // Label string (from register 0xD0+, Slot 0x02); empty if not yet received
    uint8_t state;      // 0=Off, 1=Auto, 2=On (same encoding as channel state)
    bool active;        // true if currently running
    bool configured;    // true if this slot is occupied by a configured valve
} valve_state_t;

typedef struct {
    bool on;
    bool valid;

    // Temperature setpoints (per heater): 0xE7/0xE8 (Heater 1), 0xEA/0xEB (Heater 2)
    uint8_t pool_setpoint;     // °C
    uint8_t spa_setpoint;      // °C
    uint8_t pool_setpoint_f;   // °F
    uint8_t spa_setpoint_f;    // °F
    bool setpoint_valid;       // true once a setpoint has been received
} pool_heater_t;

typedef struct {
    uint8_t timer_num;      // 1-based timer number
    uint8_t start_hour;
    uint8_t start_minute;
    uint8_t stop_hour;
    uint8_t stop_minute;
    uint8_t days;           // Bitmask: bit0=Mon, bit1=Tue, ..., bit6=Sun; 0x7F=every day
    bool valid;
} timer_state_t;

// Register label storage entry
typedef struct {
    uint8_t reg_id;      // Register identifier (byte 10 from message)
    char label[32];      // Label string
    bool valid;          // True if this entry has been populated
} register_label_t;

// Seen-device registry entry: one slot per distinct source address observed
// on the bus. Name is resolved at output time via get_device_name(); firmware
// version is populated by handle_firmware_version when a CMD 0x0A is seen.
//
// Temperature sensors hosted by a device (when it broadcasts CMD 0x16) hang
// off this entry directly. `single_sensor_source` is committed at first sight
// from the CMD 0x16 payload length (LEN 0x0D = single, LEN 0x0E = dual) and
// drives the MQTT topic shape so it stays stable across the device's lifetime.
typedef struct {
    uint8_t addr_hi;
    uint8_t addr_lo;
    bool fw_version_valid;
    uint8_t fw_version_major;
    uint8_t fw_version_minor;
    uint32_t decoded_count;     // Frames from this source that decode_message handled
    uint32_t unknown_count;     // Frames from this source that fell through to handle_unknown

    // Temperature sensors (populated from CMD 0x16; see message_decoder.c)
    uint8_t temp1;              // Sensor 1 reading in °C
    uint8_t temp2;              // Sensor 2 reading in °C (multi-sensor sources only)
    bool temp1_valid;
    bool temp2_valid;
    bool single_sensor_source;  // True if this source only ever broadcasts temp1 (LEN 0x0D)
} seen_device_t;

// Favourite/mode slot (indices 0–7: Pool, Spa, Fav1–Fav6)
typedef struct {
    char name[32];        // Label from register 0x31+index, slot 0x03
    bool enabled;         // Enable flag from register 0x21+index, slot 0x03
    bool name_valid;      // True once label has been received
    bool enabled_valid;   // True once enable flag has been received
} favourite_t;

typedef struct {
    // Temperature setpoints now live per-heater on heaters[] (pool_heater_t);
    // per-source temperature readings live on seen_device_t entries.
    bool temp_scale_fahrenheit;

    // Heaters (up to MAX_HEATERS)
    pool_heater_t heaters[MAX_HEATERS];

    // Mode
    uint8_t mode;  // 0=Spa, 1=Pool
    bool mode_valid;

    // Channels (up to MAX_CHANNELS)
    channel_state_t channels[MAX_CHANNELS];
    uint8_t num_channels;

    // Valves (up to MAX_VALVE_SLOTS)
    valve_state_t valves[MAX_VALVE_SLOTS];
    uint8_t num_valve_slots;

    // Lighting (up to MAX_LIGHT_ZONES)
    lighting_state_t lighting[MAX_LIGHT_ZONES];

    // Register labels (general storage for register names like favourites, etc.)
    register_label_t register_labels[MAX_REGISTER_LABELS];

    // Favourites / mode slots (Pool, Spa, Fav1–Fav6)
    favourite_t favourites[MAX_FAVOURITES];
    uint8_t active_favourite;    // 0x00=Pool, 0x01=Spa, 0x02-0x07=Fav1-6, 0x80=AllOff, 0x81=AllAuto
    bool active_favourite_valid;

    // Device serial number
    uint32_t serial_number;
    bool serial_number_valid;

    // Internet Gateway IP address
    uint8_t gateway_ip[4];  // IPv4 address bytes
    uint8_t gateway_signal_level;  // Signal level
    bool gateway_ip_valid;

    // Internet Gateway communications status
    uint16_t gateway_comms_status;
    bool gateway_comms_status_valid;

    // Pump
    uint16_t pump_speed;       // Current pump speed in RPM (from device 0x00A0)
    bool pump_speed_valid;

    // Chlorinator
    uint16_t ph_setpoint;      // pH * 10 (e.g., 74 = 7.4)
    uint16_t ph_reading;       // pH * 10
    uint16_t orp_setpoint;     // mV
    uint16_t orp_reading;      // mV
    bool ph_valid;
    bool orp_valid;
    uint8_t chlor_mode;        // 0=Off, 1=Auto, 2=On (tentative — see PROTOCOL.md §32)
    bool chlor_mode_valid;
    uint8_t chlor_output_level;       // Chlorine output level 1–8 (from VX 11S v3, address 0x0081)
    bool chlor_output_level_valid;
    uint8_t chlor_version_major;
    uint8_t chlor_version_minor;
    bool chlor_version_valid;

    // Controller time/clock
    uint8_t controller_minutes;
    uint8_t controller_hours;
    uint8_t controller_day_of_week; // 0=Monday, 6=Sunday
    bool controller_time_valid;

    // Touchscreen firmware version
    uint8_t touchscreen_version_major;
    uint8_t touchscreen_version_minor;
    bool touchscreen_version_valid;

    // Internet Gateway firmware version
    uint8_t gateway_version_major;
    uint8_t gateway_version_minor;
    bool gateway_version_valid;

    // Connect 8/10 Controller firmware version
    uint8_t controller_version_major;
    uint8_t controller_version_minor;
    bool controller_version_valid;

    // Seen devices (one slot per distinct source address observed)
    seen_device_t seen_devices[MAX_SEEN_DEVICES];
    uint8_t num_seen_devices;

    // Total decoded / unknown message counts across all sources
    uint32_t messages_decoded_total;
    uint32_t messages_unknown_total;

    // Timers (up to MAX_TIMERS)
    timer_state_t timers[MAX_TIMERS];

    // Last update timestamp (milliseconds since boot)
    uint32_t last_update_ms;
} pool_state_t;

// Global pool state (defined in main.c)
extern pool_state_t s_pool_state;
extern SemaphoreHandle_t s_pool_state_mutex;

#endif // POOL_STATE_H
