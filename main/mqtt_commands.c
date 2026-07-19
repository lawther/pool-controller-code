#include "mqtt_commands.h"
#include "config.h"
#include "message_decoder.h"
#include "mqtt_poolclient.h"
#include "pool_state.h"
#include "bus.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>

static const char *TAG = "MQTT_COMMANDS";

// ======================================================
// UART Command Helpers
// ======================================================

// Send raw UART message to pool bus
static void send_uart_command(const uint8_t *data, size_t len)
{
    if (bus_send_bytes(data, len) < 0) {
        ESP_LOGE(TAG, "Failed to send UART command");
    }
}

// ======================================================
// Channel Control
// ======================================================

static void handle_channel_command(int channel_id, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Channel %d command: %.*s", channel_id, payload_len, payload);

    uint8_t channel_idx = channel_id - 1;

    if (channel_idx >= MAX_CHANNELS) {
        ESP_LOGE(TAG, "Channel %d out of range", channel_id);
        return;
    }

    // Build toggle command
    // Pattern: 02 00 F0 FF FF 80 00 10 0D 8D [CHANNEL_IDX] [CHECKSUM] 03
    // Checksum = channel_idx (only data byte)
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0xFF, 0xFF, // DEST: Broadcast
        0x80, 0x00, // CONTROL
        0x10, 0x0D, 0x8D, // Command pattern
        channel_idx,       // Channel index (0-based)
        channel_idx,       // Checksum (= channel index)
        0x03               // END
    };

    ESP_LOGI(TAG, "Toggling channel %d", channel_id);
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Light Control
// ======================================================

static void handle_light_command(int zone, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Light zone %d command: %.*s", zone, payload_len, payload);

    // Determine state: OFF=0x00, AUTO=0x01, ON=0x02
    uint8_t state;
    if (strncmp(payload, "ON", payload_len) == 0) {
        state = 0x02;
    } else if (strncmp(payload, "OFF", payload_len) == 0) {
        state = 0x00;
    } else if (strncmp(payload, "AUTO", payload_len) == 0) {
        state = 0x01;
    } else {
        ESP_LOGE(TAG, "Invalid light command: %.*s (expected ON/OFF/AUTO)", payload_len, payload);
        return;
    }

    uint8_t reg_id = REG_ID_LIGHT_ZONE_STATE_0 + (zone - 1);

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 3A 0F B9 [REG_ID] 01 [STATE] [CHECKSUM] 03
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0xFF, 0xFF, // DEST: Broadcast
        0x80, 0x00, // CONTROL
        0x3A, 0x0F, 0xB9, // Command pattern
        reg_id,     // Register ID (light zone)
        0x01,       // Slot ID (state)
        state,      // State value (OFF/AUTO/ON)
        0x00,       // Checksum (calculated below)
        0x03        // END
    };

    // Calculate checksum (sum of bytes 10-12)
    cmd[13] = (reg_id + 0x01 + state) & 0xFF;

    ESP_LOGI(TAG, "Sending light zone %d %s command", zone,
             state == 0x02 ? "ON" : (state == 0x00 ? "OFF" : "AUTO"));
    send_uart_command(cmd, sizeof(cmd));
}

static void handle_light_color_command(int zone, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Light zone %d color command: %.*s", zone, payload_len, payload);

    // Map the color name (an HA effect_list entry) back to its code in the
    // shared color table; the discovery effect list constrains what HA offers
    uint8_t color = 0;
    for (int i = 1; i < LIGHTING_COLOR_COUNT; i++) {
        if ((int)strlen(LIGHTING_COLOR_NAMES[i]) == payload_len &&
            strncmp(payload, LIGHTING_COLOR_NAMES[i], payload_len) == 0) {
            color = (uint8_t)i;
            break;
        }
    }
    if (color == 0) {
        ESP_LOGE(TAG, "Unknown light color: %.*s", payload_len, payload);
        return;
    }

    uint8_t reg_id = REG_ID_LIGHT_ZONE_COLOR_0 + (zone - 1);

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 3A 0F B9 [REG_ID] 01 [COLOR] [CHECKSUM] 03
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0xFF, 0xFF, // DEST: Broadcast
        0x80, 0x00, // CONTROL
        0x3A, 0x0F, 0xB9, // Command pattern
        reg_id,     // Register ID (light zone color)
        0x01,       // Slot ID
        color,      // Color code (shared color table)
        0x00,       // Checksum (calculated below)
        0x03        // END
    };

    // Calculate checksum (sum of bytes 10-12)
    cmd[13] = (reg_id + 0x01 + color) & 0xFF;

    ESP_LOGI(TAG, "Sending light zone %d color %s (0x%02X) command", zone,
             LIGHTING_COLOR_NAMES[color], color);
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Heater Control
// ======================================================

static void handle_heater_command(const char *payload, int payload_len, int index)
{
    ESP_LOGI(TAG, "Heater %d command: %.*s", index, payload_len, payload);

    // State register per heater: Heater 1 = 0xE6, Heater 2 = 0xE9.
    uint8_t reg;
    if (index == 0) {
        reg = REG_ID_HEATER1_ONOFF;
    } else if (index == 1) {
        reg = REG_ID_HEATER2_ONOFF;
    } else {
        ESP_LOGW(TAG, "No state register known for heater %d", index);
        return;
    }

    uint8_t state;
    if (strncmp(payload, "ON", payload_len) == 0) {
        state = 0x01;
    } else if (strncmp(payload, "OFF", payload_len) == 0) {
        state = 0x00;
    } else {
        ESP_LOGE(TAG, "Invalid heater command: %.*s (expected ON/OFF)", payload_len, payload);
        return;
    }

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 3A 0F B9 [REG] 00 [STATE] [CHECKSUM] 03
    // Checksum = reg + 0x00 + state
    uint8_t cmd[] = {
        0x02,             // START
        0x00, 0xF0,       // SOURCE: Internet Gateway
        0xFF, 0xFF,       // DEST: Broadcast
        0x80, 0x00,       // CONTROL
        0x3A, 0x0F, 0xB9, // Command pattern
        reg,              // Register ID (heater state)
        0x00,             // Slot
        state,            // State (0x00=Off, 0x01=On)
        (reg + 0x00 + state) & 0xFF, // Checksum
        0x03              // END
    };

    ESP_LOGI(TAG, "Sending heater %d %s command", index, state ? "ON" : "OFF");
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Favourite Control
// ======================================================

static void handle_favourite_command(const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Favourite command: %.*s", payload_len, payload);

    uint8_t value;
    if (strncmp(payload, "Pool", payload_len) == 0 && payload_len == 4) {
        value = FAVOURITE_POOL;
    } else if (strncmp(payload, "Spa", payload_len) == 0 && payload_len == 3) {
        value = FAVOURITE_SPA;
    } else if (strncmp(payload, "All Off", payload_len) == 0 && payload_len == 7) {
        value = FAVOURITE_ALL_OFF;
    } else if (strncmp(payload, "All Auto", payload_len) == 0 && payload_len == 8) {
        value = FAVOURITE_ALL_AUTO;
    } else if (strncmp(payload, "No Favourite", payload_len) == 0 && payload_len == 12) {
        // Status-only value (register 0x20 = 0xFF, no favourite active).
        // There is no bus command to deactivate a favourite: CMD 0x2A with
        // value 0xFF was tested and the controller ignores it.
        ESP_LOGI(TAG, "Ignoring 'No Favourite' selection (status-only)");
        return;
    } else {
        // Search enabled user favourites (indices 2–7 → values 0x02–0x07)
        value = 0xFF;
        if (s_pool_state_mutex &&
            xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            for (int i = 2; i < MAX_FAVOURITES; i++) {
                const favourite_t *fav = &s_pool_state.favourites[i];
                if (fav->enabled_valid && fav->enabled &&
                    fav->name_valid &&
                    (int)strlen(fav->name) == payload_len &&
                    strncmp(payload, fav->name, payload_len) == 0) {
                    value = (uint8_t)i;
                    break;
                }
            }
            xSemaphoreGive(s_pool_state_mutex);
        }
        if (value == 0xFF) {
            ESP_LOGE(TAG, "Unknown favourite: %.*s", payload_len, payload);
            return;
        }
    }

    // Build CMD 0x2A to Touchscreen (0x0050)
    // Pattern: 02 00 F0 00 50 80 00 2A 0D F9 [VALUE] [CHECKSUM] 03
    // Checksum = value (only data byte)
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0x00, 0x50, // DEST: Touchscreen
        0x80, 0x00, // CONTROL
        0x2A, 0x0D, 0xF9, // Command pattern
        value,      // Favourite value
        value,      // Checksum (= value)
        0x03        // END
    };

    ESP_LOGI(TAG, "Sending favourite command 0x%02X", value);
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Mode Control
// ======================================================

static void handle_mode_command(const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Mode command: %.*s", payload_len, payload);

    // CMD 0x15 uses the same encoding as the 0x14 status: MODE_SPA/MODE_POOL
    uint8_t mode_value;
    if (strncmp(payload, "Pool", payload_len) == 0) {
        mode_value = MODE_POOL;
    } else if (strncmp(payload, "Spa", payload_len) == 0) {
        mode_value = MODE_SPA;
    } else {
        ESP_LOGE(TAG, "Invalid mode command: %.*s (expected Pool/Spa)", payload_len, payload);
        return;
    }

    // Build CMD 0x15 Mode Set broadcast, impersonating the Touchscreen
    // Pattern: 02 00 50 FF FF 80 00 15 0D F2 [MODE] [CHECKSUM] 03
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0x50, // SOURCE: Touchscreen
        0xFF, 0xFF, // DEST: Broadcast
        0x80, 0x00, // CONTROL
        0x15, 0x0D, 0xF2, // Command pattern
        mode_value, // Mode value (MODE_SPA or MODE_POOL)
        mode_value, // Checksum (just the mode value)
        0x03        // END
    };

    ESP_LOGI(TAG, "Sending mode switch to %s", mode_value == MODE_POOL ? "Pool" : "Spa");
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Temperature Setpoint Control
// ======================================================

static void handle_temperature_command(bool is_pool, const char *payload, int payload_len)
{
    // Parse temperature value (Celsius)
    char temp_str[16];
    if (payload_len >= sizeof(temp_str)) {
        ESP_LOGE(TAG, "Temperature payload too long");
        return;
    }
    memcpy(temp_str, payload, payload_len);
    temp_str[payload_len] = '\0';

    char *endptr;
    long temp_parsed = strtol(temp_str, &endptr, 10);
    if (endptr == temp_str || *endptr != '\0') {
        ESP_LOGE(TAG, "Invalid temperature value: \"%s\"", temp_str);
        return;
    }
    ESP_LOGI(TAG, "%s setpoint command: %ld°C", is_pool ? "Pool" : "Spa", temp_parsed);

    // Validate temperature range (Celsius)
    if (temp_parsed < TEMP_SETPOINT_MIN_C || temp_parsed > TEMP_SETPOINT_MAX_C) {
        ESP_LOGE(TAG, "Temperature out of range: %ld°C (valid: %d-%d)",
                 temp_parsed, TEMP_SETPOINT_MIN_C, TEMP_SETPOINT_MAX_C);
        return;
    }
    int32_t temp_c = (int32_t)temp_parsed;  // safe: range-checked above

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 19 0F 98 [TARGET] [TEMP_C] [TEMP_C] [CHECKSUM] 03
    // TARGET: 0x01=Pool, 0x02=Spa
    // Temperature byte is repeated as part of the message format
    // Checksum = TARGET + TEMP_C + TEMP_C
    uint8_t target = is_pool ? 0x01 : 0x02;
    uint8_t temp_byte = (uint8_t)temp_c;
    uint8_t checksum = (target + temp_byte + temp_byte) & 0xFF;

    uint8_t cmd[] = {
        0x02,             // START
        0x00, 0xF0,       // SOURCE: Internet Gateway
        0xFF, 0xFF,       // DEST: Broadcast
        0x80, 0x00,       // CONTROL
        0x19, 0x0F, 0x98, // Command pattern
        target,           // Target (0x01=Pool, 0x02=Spa)
        temp_byte,        // Temperature °C
        temp_byte,        // Temperature °C (repeated)
        checksum,         // Checksum
        0x03              // END
    };

    ESP_LOGI(TAG, "Setting %s setpoint to %" PRId32 "°C", is_pool ? "pool" : "spa", temp_c);
    send_uart_command(cmd, sizeof(cmd));
}

// Per-heater setpoint write.
//  - Heater 1 (index 0): the system pool/spa setpoint command (CMD 0x19).
//  - Heater 2 (index 1): gateway register write (CMD 0x3A) to 0xEA (pool) / 0xEB (spa).
static void handle_heater_setpoint_command(int index, bool is_pool,
                                           const char *payload, int payload_len)
{
    if (index == 0) {
        handle_temperature_command(is_pool, payload, payload_len);
        return;
    }
    // index == 1 (Heater 2): gateway register write to 0xEA (pool) / 0xEB (spa).

    // Parse temperature value (Celsius)
    char temp_str[16];
    if (payload_len >= (int)sizeof(temp_str)) {
        ESP_LOGE(TAG, "Temperature payload too long");
        return;
    }
    memcpy(temp_str, payload, payload_len);
    temp_str[payload_len] = '\0';

    char *endptr;
    long temp_parsed = strtol(temp_str, &endptr, 10);
    if (endptr == temp_str || *endptr != '\0') {
        ESP_LOGE(TAG, "Invalid temperature value: \"%s\"", temp_str);
        return;
    }
    if (temp_parsed < TEMP_SETPOINT_MIN_C || temp_parsed > TEMP_SETPOINT_MAX_C) {
        ESP_LOGE(TAG, "Temperature out of range: %ld°C (valid: %d-%d)",
                 temp_parsed, TEMP_SETPOINT_MIN_C, TEMP_SETPOINT_MAX_C);
        return;
    }

    uint8_t reg  = is_pool ? 0xEA : 0xEB;   // Heater 2 pool / spa setpoint register
    uint8_t slot = 0x00;
    uint8_t val  = (uint8_t)temp_parsed;

    // Pattern: 02 00 F0 FF FF 80 00 3A 0F B9 [REG] [SLOT] [VAL] [CHECKSUM] 03
    uint8_t cmd[] = {
        0x02,             // START
        0x00, 0xF0,       // SOURCE: Internet Gateway
        0xFF, 0xFF,       // DEST: Broadcast
        0x80, 0x00,       // CONTROL
        0x3A, 0x0F, 0xB9, // Register write command pattern
        reg,              // Register ID
        slot,             // Slot
        val,              // Value °C
        (reg + slot + val) & 0xFF, // Checksum
        0x03              // END
    };

    ESP_LOGI(TAG, "Setting Heater 2 %s setpoint to %ld°C", is_pool ? "pool" : "spa", temp_parsed);
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Valve Control
// ======================================================

static void handle_valve_command(int valve_num, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Valve %d command: %.*s", valve_num, payload_len, payload);

    uint8_t state;
    if (strncmp(payload, "On", payload_len) == 0) {
        state = 0x02;
    } else if (strncmp(payload, "Auto", payload_len) == 0) {
        state = 0x01;
    } else if (strncmp(payload, "Off", payload_len) == 0) {
        state = 0x00;
    } else {
        ESP_LOGE(TAG, "Invalid valve command: %.*s (expected Off/Auto/On)", payload_len, payload);
        return;
    }

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 28 0E A6 [VALVE_IDX] [STATE] [CHECKSUM] 03
    // Checksum = (valve_idx + state) & 0xFF
    uint8_t valve_idx = valve_num - 1;
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0xFF, 0xFF, // DEST: Broadcast
        0x80, 0x00, // CONTROL
        0x28, 0x0E, 0xA6, // Command pattern (checksum: 02+00+F0+FF+FF+80+00+28+0E = 0xA6)
        valve_idx,         // Valve index (0-based)
        state,             // Target state (0=Off, 1=Auto, 2=On)
        (valve_idx + state) & 0xFF, // Data checksum
        0x03               // END
    };

    ESP_LOGI(TAG, "Setting valve %d to %s", valve_num,
             state == 0x02 ? "On" : (state == 0x01 ? "Auto" : "Off"));
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Main Command Handler
// ======================================================

void mqtt_handle_command(const char *topic, int topic_len, const char *data, int data_len)
{
    // Get device ID for topic matching
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    // Build expected topic prefix
    char topic_prefix[64];
    snprintf(topic_prefix, sizeof(topic_prefix), "pool/%s/", device_id);
    int prefix_len = strlen(topic_prefix);

    // Check if topic matches our device
    if (topic_len < prefix_len || strncmp(topic, topic_prefix, prefix_len) != 0) {
        ESP_LOGW(TAG, "Topic does not match device ID");
        return;
    }

    // Extract command part (after device prefix)
    const char *cmd_topic = topic + prefix_len;
    int cmd_topic_len = topic_len - prefix_len;

    // Parse command type
    if (strncmp(cmd_topic, "channel/", 8) == 0 && cmd_topic_len > 12) {
        // Extract channel number (format: "channel/N/set")
        char *endptr;
        int channel = (int)strtol(cmd_topic + 8, &endptr, 10);
        if (endptr == cmd_topic + 8 || *endptr != '/') {
            ESP_LOGE(TAG, "Invalid channel topic format: %s", cmd_topic);
            return;
        }
        if (channel >= 1 && channel <= MAX_CHANNELS) {
            handle_channel_command(channel, data, data_len);
        } else {
            ESP_LOGE(TAG, "Invalid channel number: %d", channel);
        }
    }
    else if (strncmp(cmd_topic, "light/", 6) == 0 && cmd_topic_len > 10) {
        // Extract zone number (formats: "light/N/set", "light/N/color/set")
        char *endptr;
        int zone = (int)strtol(cmd_topic + 6, &endptr, 10);
        if (endptr == cmd_topic + 6 || *endptr != '/') {
            ESP_LOGE(TAG, "Invalid light topic format: %s", cmd_topic);
            return;
        }
        if (zone < 1 || zone > MAX_LIGHT_ZONES) {
            ESP_LOGE(TAG, "Invalid light zone: %d", zone);
            return;
        }
        int suffix_len = cmd_topic_len - (int)(endptr - cmd_topic);
        if (suffix_len == 4 && strncmp(endptr, "/set", 4) == 0) {
            handle_light_command(zone, data, data_len);
        } else if (suffix_len == 10 && strncmp(endptr, "/color/set", 10) == 0) {
            handle_light_color_command(zone, data, data_len);
        } else {
            ESP_LOGE(TAG, "Invalid light topic format: %s", cmd_topic);
        }
    }
    else if (strncmp(cmd_topic, "valve/", 6) == 0 && cmd_topic_len > 8) {
        // Extract valve number (format: "valve/N/set")
        char *endptr;
        int valve = (int)strtol(cmd_topic + 6, &endptr, 10);
        if (endptr == cmd_topic + 6 || *endptr != '/') {
            ESP_LOGE(TAG, "Invalid valve topic format: %s", cmd_topic);
            return;
        }
        if (valve >= 1 && valve <= MAX_VALVE_SLOTS) {
            handle_valve_command(valve, data, data_len);
        } else {
            ESP_LOGE(TAG, "Invalid valve number: %d", valve);
        }
    }
    else if (strncmp(cmd_topic, "heater/", 7) == 0) {
        // Suffixes: "/set" (on/off), "/pool_setpoint/set", "/spa_setpoint/set"
        char *endptr;
        int idx = (int)strtol(cmd_topic + 7, &endptr, 10);
        int suffix_len = cmd_topic_len - (int)(endptr - cmd_topic);
        if (endptr == cmd_topic + 7 || idx < 0 || idx >= MAX_HEATERS) {
            ESP_LOGE(TAG, "Invalid heater topic: %s", cmd_topic);
        } else if (suffix_len == 4 && strncmp(endptr, "/set", 4) == 0) {
            handle_heater_command(data, data_len, idx);
        } else if (suffix_len == 18 && strncmp(endptr, "/pool_setpoint/set", 18) == 0) {
            handle_heater_setpoint_command(idx, true, data, data_len);
        } else if (suffix_len == 17 && strncmp(endptr, "/spa_setpoint/set", 17) == 0) {
            handle_heater_setpoint_command(idx, false, data, data_len);
        } else {
            ESP_LOGE(TAG, "Invalid heater topic: %s", cmd_topic);
        }
    }
    else if (strncmp(cmd_topic, "mode/set", 8) == 0) {
        handle_mode_command(data, data_len);
    }
    else if (strncmp(cmd_topic, "favourite/set", 13) == 0) {
        handle_favourite_command(data, data_len);
    }
    else {
        ESP_LOGW(TAG, "Unknown command topic: %.*s", cmd_topic_len, cmd_topic);
    }
}
