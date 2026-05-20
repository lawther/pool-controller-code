#include "message_decoder.h"
#include "config.h"
#include "mqtt_publish.h"
#include "register_requester.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "MSG_DECODER";

// ======================================================
// Little-endian byte-to-word conversion helpers
// ======================================================

// Read 16-bit little-endian value from buffer at offset
#define UINT16_LE(ptr, offset) ((uint16_t)((ptr)[(offset)] | ((ptr)[(offset)+1] << 8)))

// Read 32-bit little-endian value from buffer at offset
#define UINT32_LE(ptr, offset) ((uint32_t)((ptr)[(offset)] | ((ptr)[(offset)+1] << 8) | \
                                            ((ptr)[(offset)+2] << 16) | ((ptr)[(offset)+3] << 24)))

// ======================================================
// Helper function for pattern matching
// ======================================================

/**
 * Match data against a hex string pattern (e.g. "02 00 50 FF FF")
 * Returns true if data matches pattern
 */
static bool match_pattern(const uint8_t *data, int data_len, const char *pattern)
{
    int data_idx = 0;
    const char *p = pattern;

    while (*p && data_idx < data_len) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        // Parse two hex digits
        if (!*p || !*(p + 1)) return false;  // Need 2 hex digits

        char hex[3] = {p[0], p[1], 0};
        unsigned long expected = strtoul(hex, NULL, 16);

        if (data[data_idx] != (uint8_t)expected) {
            return false;
        }

        data_idx++;
        p += 2;
    }

    return true;  // All pattern bytes matched
}

// ======================================================
// Message type patterns (as hex strings for readability)
// ======================================================

// Register data (CMD 0x38) is dispatched source-agnostically in
// dispatch_message() — see PROTOCOL.md command `0x38`. Routing is by
// (reg_id, slot) via REGISTER_HANDLERS, independent of the source device.

// Message type patterns (messages start with 0x02, end with 0x03)
// 50 Main Controller (Connect 10)
static const char *MSG_TYPE_TEMP_SETTING =          "02 00 50 FF FF 80 00 17 10 F7";
static const char *MSG_TYPE_CONFIG =                "02 00 50 FF FF 80 00 26 0E 04";
static const char *MSG_TYPE_MODE =                  "02 00 50 FF FF 80 00 14 0D F1";
static const char *MSG_TYPE_CHANNELS =              "02 00 50 00 6F 80 00 0D 0D 5B";
static const char *MSG_TYPE_CHANNEL_STATUS =        "02 00 50 FF FF 80 00 0B 25 00";
static const char *MSG_TYPE_LIGHT_CONFIG =          "02 00 50 FF FF 80 00 06 0E E4";
static const char *MSG_TYPE_CONTROLLER_TIME =       "02 00 50 FF FF 80 00 FD 0F DC";
static const char *MSG_TYPE_TOUCHSCREEN_UNKNOWN1 =  "02 00 50 FF FF 80 00 12 0E F0";
static const char *MSG_TYPE_TOUCHSCREEN_UNKNOWN2 =  "02 00 50 FF FF 80 00 27 0D 04";
static const char *MSG_TYPE_TOUCHSCREEN_UNKNOWN3 =  "02 00 50 FF FF 80 00 05 0D E2";
static const char *MSG_TYPE_VALVE_STATE =           "02 00 50 FF FF 80 00 27 13 0A";

// Water temperature reading (CMD 0x16) is dispatched source-agnostically in
// dispatch_message() — see PROTOCOL.md command `0x16`. Observed from the
// Connect 8/10 Controller (0x0062, LEN 0x0E, 2-byte payload temp1+temp2),
// the Genus Heater family (0x0070/0x0072, LEN 0x0D, 1-byte payload temp1 only),
// and the ICI Gas Heater (0x0074, LEN 0x0D, 1-byte payload temp1 only).

// 62 Connect 8/10 Controller
static const char *MSG_TYPE_HEATER =                "02 00 62 FF FF 80 00 12 0F 03";

// 70 Genus Heater (Active i25 Evo)
static const char *MSG_TYPE_GENUS_HEATER_TEMP_SETTING = "02 00 70 FF FF 80 00 17 0E 15";

// 74 ICI Gas Heater (Astral/Fluidra ICI 400B NG)
// CMD 0x16 (temperature reading) is handled by the source-agnostic handler above.
static const char *MSG_TYPE_ICI_HEATER_STATUS =       "02 00 74 FF FF 80 00 12 10 16";
static const char *MSG_TYPE_ICI_HEATER_TEMP_SETTING = "02 00 74 FF FF 80 00 17 0E 19";

// Chlorinator setpoints (CMD 0x1D) and readings (CMD 0x1F) are dispatched
// source-agnostically in dispatch_message() — see PROTOCOL.md commands `0x1D`
// and `0x1F`. Same payload shape from both 0x0090 RolaChem and 0x0084 Viron.

// Chlorinator status broadcast (CMD 0x12) — same payload shape from either variant
static const char *MSG_TYPE_CHLOR_STATUS_A = "02 00 90 FF FF 80 00 12 0D 2F";
static const char *MSG_TYPE_CHLOR_STATUS_B = "02 00 84 FF FF 80 00 12 0D 23";

// F0 Internet Gateway
static const char *MSG_TYPE_SERIAL_NUMBER =           "02 00 F0 FF FF 80 00 37 11 B8";
static const char *MSG_TYPE_GATEWAY_IP =              "02 00 F0 FF FF 80 00 37 15 BC";
static const char *MSG_TYPE_GATEWAY_COMMS =           "02 00 F0 FF FF 80 00 37 0F B6";
static const char *MSG_TYPE_GATEWAY_STATUS =          "02 00 F0 FF FF 80 00 12 0F 91";

// Register read request (CMD 0x39) is dispatched source-agnostically in
// dispatch_message() — see PROTOCOL.md command `0x39`. Observed from the
// Internet Gateway (0x00F0) and the Genus Heater (0x0070); payload shape
// `{reg_id, slot_id}` is identical.

// Temperature setpoint command (CMD 0x19) is dispatched source-agnostically
// in dispatch_message() — see PROTOCOL.md command `0x19`. Slot byte (payload[0])
// selects Pool/Spa (0x01/0x02, 3-byte payload from 0x00F0 Gateway) or heater
// pair (0x03, 5-byte payload from 0x0050 Touch Screen to 0x007F).

// F0 Gateway Control Commands (Gateway -> Controller)
static const char *MSG_TYPE_CHANNEL_TOGGLE_CMD =      "02 00 F0 FF FF 80 00 10 0D 8D";
static const char *MSG_TYPE_LIGHT_CONTROL_CMD =       "02 00 F0 FF FF 80 00 3A 0F B9";
static const char *MSG_TYPE_MODE_CONTROL_CMD =        "02 00 F0 00 50 80 00 2A 0D F9";

// ======================================================
// Lookup tables and constants
// ======================================================

// Channel type lookup table
typedef struct {
    uint8_t code;
    const char *name;
} channel_type_entry_t;

static const channel_type_entry_t CHANNEL_TYPE_TABLE[] = {
    {0x00, "Unused"},
    {0x01, "Filter"},
    {0x02, "Cleaning"},
    {0x03, "Heater Pump"},
    {0x04, "Booster"},
    {0x05, "Waterfall"},
    {0x06, "Fountain"},
    {0x07, "Spa Pump"},
    {0x08, "Solar"},
    {0x09, "Blower"},
    {0x0A, "Swimjet"},
    {0x0B, "Jets"},
    {0x0C, "Spa Jets"},
    {0x0D, "Overflow"},
    {0x0E, "Spillway"},
    {0x0F, "Audio"},
    {0x10, "Hot Seat"},
    {0x11, "Heater Power"},
    {0x12, "Custom Name"},
    {0xFB, "Secondary Heater"},
    {0xFD, "Heater"},
    {0xFE, "Light Zone"},
};

#define CHANNEL_TYPE_TABLE_SIZE (sizeof(CHANNEL_TYPE_TABLE) / sizeof(CHANNEL_TYPE_TABLE[0]))

/**
 * Get channel type name from type code
 * @param type_code Channel type code (0x00-0x12, 0xFD, 0xFE)
 * @return Channel type name, or "Unknown" if not found
 */
const char* get_channel_type_name(uint8_t type_code) {
    for (int i = 0; i < CHANNEL_TYPE_TABLE_SIZE; i++) {
        if (CHANNEL_TYPE_TABLE[i].code == type_code) {
            return CHANNEL_TYPE_TABLE[i].name;
        }
    }
    return "Unknown";
}

// Command byte name lookup table — used to annotate unhandled messages in logs.
// Some CMD bytes are source-dependent (e.g. 0x12 from 0x0050 vs 0x00F0 vs
// 0x0062); the labels here are generic. See PROTOCOL.md for the full
// per-source semantics.
typedef struct {
    uint8_t cmd;
    const char *name;
} cmd_name_entry_t;

static const cmd_name_entry_t CMD_NAME_TABLE[] = {
    {0x05, "Touchscreen Unknown 3"},
    {0x06, "Lighting Zone Config"},
    {0x0A, "Firmware Version"},
    {0x0B, "Channel Status"},
    {0x0D, "Active Channels Bitmask"},
    {0x10, "Channel Toggle Cmd"},
    {0x12, "Status/Other"},
    {0x14, "Mode"},
    {0x16, "Temperature Reading"},
    {0x17, "Temperature Setting"},
    {0x18, "Chlorinator Cell Mode"},
    {0x19, "Temp Set Cmd"},
    {0x1D, "Chlorinator Setpoint"},
    {0x1F, "Chlorinator Reading"},
    {0x25, "Valve Sync"},
    {0x26, "Configuration"},
    {0x27, "Valve State"},
    {0x2A, "Mode/Favourite Cmd"},
    {0x31, "Temperature Reading (alt)"},
    {0x37, "Gateway Info Req/Resp"},
    {0x38, "Register Response"},
    {0x39, "Register Request"},
    {0x3A, "Light Zone Control Cmd"},
    {0xFD, "Controller Day/Time"},
};

#define CMD_NAME_TABLE_SIZE (sizeof(CMD_NAME_TABLE) / sizeof(CMD_NAME_TABLE[0]))

/**
 * Get a generic name for a CMD byte.
 *
 * If the CMD is in the table, returns the static table label.
 * Otherwise formats "Unknown CMD 0xXX" into the caller-supplied buffer and
 * returns a pointer to it. The buffer must be at least 20 bytes.
 *
 * Labels are intentionally generic since many CMDs are source-dependent.
 */
static const char* get_cmd_name(uint8_t cmd, char *fallback_buf, size_t buf_size) {
    for (int i = 0; i < CMD_NAME_TABLE_SIZE; i++) {
        if (CMD_NAME_TABLE[i].cmd == cmd) {
            return CMD_NAME_TABLE[i].name;
        }
    }
    snprintf(fallback_buf, buf_size, "Unknown CMD 0x%02X", cmd);
    return fallback_buf;
}

// Channel state names
const char *CHANNEL_STATE_NAMES[] = {
    "Off",          // 0
    "Auto",         // 1
    "On",           // 2
    "Low Speed",    // 3
    "Medium Speed", // 4
    "High Speed",   // 5
};

// Lighting state names
const char *LIGHTING_STATE_NAMES[] = {
    "Off",          // 0
    "Auto",         // 1
    "On",           // 2
};

// Lighting zone preset name lookup table
const char *LIGHT_ZONE_NAME_TABLE[] = {
    "Pool",         // 0x00
    "Spa",          // 0x01
    "Pool & Spa",   // 0x02
    "Waterfall 1",  // 0x03
    "Waterfall 2",  // 0x04
    "Waterfall 3",  // 0x05
};

// Day of week names
const char *DAY_OF_WEEK_NAMES[] = {
    "Monday",       // 0
    "Tuesday",      // 1
    "Wednesday",    // 2
    "Thursday",     // 3
    "Friday",       // 4
    "Saturday",     // 5
    "Sunday",       // 6
};
#define DAY_OF_WEEK_COUNT 7

// Lighting color names
const char *LIGHTING_COLOR_NAMES[] = {
    "Unknown",           // 0
    "Red",               // 1
    "Orange",            // 2
    "Yellow",            // 3
    "Green",             // 4
    "Blue",              // 5
    "Purple",            // 6
    "White",             // 7
    "User 1",            // 8
    "User 2",            // 9
    "Disco",             // 10
    "Smooth",            // 11
    "Fade",              // 12
    "Magenta",           // 13
    "Cyan",              // 14
    "Pattern",           // 15
    "Rainbow",           // 16
    "Ocean",             // 17
    "Voodoo Lounge",     // 18
    "Deep Blue Sea",     // 19
    "Royal Blue",        // 20
    "Afternoon Skies",   // 21
    "Aqua Green",        // 22
    "Emerald",           // 23
    "Warm Red",          // 24
    "Flamingo",          // 25
    "Vivid Violet",      // 26
    "Sangria",           // 27
    "Twilight",          // 28
    "Tranquillity",      // 29
    "Gemstone",          // 30
    "USA",               // 31
    "Mardi Gras",        // 32
    "Cool Cabaret",      // 33
    "Sam",               // 34
    "Party",             // 35
    "Romance",           // 36
    "Caribbean",         // 37
    "American",          // 38
    "California Sunset", // 39
    "Royal",             // 40
    "Hold",              // 41
    "Recall",            // 42
    "Peruvian Paradise", // 43
    "Super Nova",        // 44
    "Northern Lights",   // 45
    "Tidal Wave",        // 46
    "Patriot Dream",     // 47
    "Desert Skies",      // 48
    "Nova",              // 49
    "Pink",              // 50
};

// Gateway comms status lookup table
typedef struct {
    uint16_t code;
    const char *text;
} gateway_comms_status_t;

static const gateway_comms_status_t GATEWAY_COMMS_STATUS[] = {
    {0, "Idle"},
    {256, "No suitable interfaces ready"},
    {513, "DNS resolve error"},
    {769, "Internal error creating local socket"},
    {1024, "Connecting to server"},
    {1025, "Failed to connect"},
    {32768, "Connection open"},
    {32769, "Communicating with server"},
    {61440, "Connection closed"},
    {61441, "Communication error with server"},
    {61442, "Communication error with server"},
    {61443, "Communication error with server"},
    {61444, "Communication error with server"},
    // Add more status codes here as they are discovered
};
#define GATEWAY_COMMS_STATUS_COUNT (sizeof(GATEWAY_COMMS_STATUS) / sizeof(GATEWAY_COMMS_STATUS[0]))

// ======================================================
// Helper functions
// ======================================================

// Callback type for state update functions
typedef void (*state_update_fn)(pool_state_t *state, void *data);

/**
 * Update pool state with mutex protection and optionally publish to MQTT
 *
 * @param ctx Message decoder context
 * @param update_fn Function to update state (called with mutex held)
 * @param update_data Data to pass to update function
 * @param publish_fn MQTT publish function to call (or NULL for no publishing)
 * @return true if state was updated successfully
 */
static bool update_state_and_publish(
    message_decoder_context_t *ctx,
    state_update_fn update_fn,
    void *update_data,
    void (*publish_fn)(const pool_state_t*))
{
    if (!ctx || !ctx->state_mutex) {
        return false;
    }

    pool_state_t snapshot;
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        // Call update function with mutex held
        update_fn(ctx->pool_state, update_data);

        // Update timestamp
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // Take snapshot before releasing mutex
        snapshot = *ctx->pool_state;

        xSemaphoreGive(ctx->state_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire mutex for state update");
        return false;
    }

    // Publish to MQTT (outside mutex to avoid blocking)
    if (ctx->enable_mqtt && publish_fn) {
        publish_fn(&snapshot);
    }

    return true;
}

/**
 * Update pool state without MQTT publishing
 *
 * @param ctx Message decoder context
 * @param update_fn Function to update state (called with mutex held)
 * @param update_data Data to pass to update function
 * @return true if state was updated successfully
 */
static bool update_state_only(
    message_decoder_context_t *ctx,
    state_update_fn update_fn,
    void *update_data)
{
    return update_state_and_publish(ctx, update_fn, update_data, NULL);
}

const char* get_device_name(uint8_t addr_hi, uint8_t addr_lo, char *fallback_buf, size_t buf_size)
{
    if (addr_hi == 0xFF && addr_lo == 0xFF) return "Broadcast";
    if (addr_hi == 0x00) {
        switch (addr_lo) {
            case 0x50: return "Touch Screen";
            case 0x62: return "Connect 8/10";
            case 0x6F: return "Internal Channels";
            case 0x70: return "Genus Heater";
            case 0x74: return "ICI Gas Heater";
            case 0x84: return "Viron Chlorinator";
            case 0x90: return "RolaChem";
            case 0xA0: return "Internal Salt Cell";
            case 0xF0: return "Internet Gateway";
        }
    }
    snprintf(fallback_buf, buf_size, "Unknown 0x%02X%02X", addr_hi, addr_lo);
    return fallback_buf;
}

const char* get_device_slug(uint8_t addr_hi, uint8_t addr_lo, char *buf, size_t buf_size)
{
    if (buf_size == 0) return buf;

    if (addr_hi == 0xFF && addr_lo == 0xFF) {
        snprintf(buf, buf_size, "broadcast");
        return buf;
    }

    char name_buf[16];
    const char *name = get_device_name(addr_hi, addr_lo, name_buf, sizeof(name_buf));

    // Unknown sources: skip the "0x" prefix that would otherwise leak into the slug.
    if (strncmp(name, "Unknown ", 8) == 0) {
        snprintf(buf, buf_size, "unknown_%02x%02x", addr_hi, addr_lo);
        return buf;
    }

    // Slugify: lowercase alphanumerics pass through; everything else collapses to a single underscore.
    size_t pos = 0;
    bool last_was_underscore = true;  // start true to suppress a leading underscore
    for (const char *p = name; *p && pos + 1 < buf_size; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            buf[pos++] = c - 'A' + 'a';
            last_was_underscore = false;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            buf[pos++] = c;
            last_was_underscore = false;
        } else if (!last_was_underscore) {
            buf[pos++] = '_';
            last_was_underscore = true;
        }
    }
    if (pos > 0 && buf[pos - 1] == '_') pos--;  // strip trailing underscore
    buf[pos] = '\0';
    return buf;
}

const char* get_gateway_comms_status_text(uint16_t code)
{
    for (int i = 0; i < GATEWAY_COMMS_STATUS_COUNT; i++) {
        if (GATEWAY_COMMS_STATUS[i].code == code) {
            return GATEWAY_COMMS_STATUS[i].text;
        }
    }
    return "Unknown";
}

bool verify_message_checksum(const uint8_t *data, int len)
{
    // Must have at least: 02 [10 bytes] [data] [checksum] 03
    if (len < 13 || data[0] != 0x02 || data[len - 1] != 0x03) {
        return false;
    }

    // Calculate checksum: sum bytes from index 10 to (len-3) inclusive
    uint32_t sum = 0;
    for (int i = 10; i < len - 2; i++) {
        sum += data[i];
    }

    uint8_t calculated_checksum = sum & 0xFF;
    uint8_t received_checksum = data[len - 2];

    return (calculated_checksum == received_checksum);
}

// ======================================================
// Message handler functions
// ======================================================

/**
 * Message handler function signature
 * Returns true if message was handled successfully
 */
typedef bool (*message_handler_fn)(
    const uint8_t *data,
    int len,
    const uint8_t *payload,
    int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx);

/**
 * Register handler dispatch table entry
 * Maps (register_range, slot) to handler function
 */
typedef struct {
    uint8_t reg_start;      // Start of register range
    uint8_t reg_end;        // End of register range (inclusive)
    uint8_t slot;           // Data slot identifier
    message_handler_fn handler;
    const char *name;       // For logging
} register_handler_t;

// Forward declarations for register handlers
static bool handle_timer(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_type(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_name(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_color(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_active(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_multicolor(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_light_zone_name(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_valve_label(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_favourite_label(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_favourite_enable(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_temp_setpoint(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_channel_count(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_valve_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_touchscreen_unknown3(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_heater1_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);
static bool handle_heater2_state(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);

/**
 * Register message dispatch table
 * Entries are checked in order, first match wins
 * Only includes register ranges with confirmed behavior
 */
static const register_handler_t REGISTER_HANDLERS[] = {
    // Timers (slot 0x04, registers 0x08-0x17 = timers 1-16)
    {0x08, 0x17, 0x04, handle_timer,              "Timer"},

    // Channel configuration
    {0x6C, 0x73, 0x02, handle_channel_type,       "Channel Type"},
    {0x7C, 0x83, 0x02, handle_channel_name,       "Channel Name"},
    {0x8C, 0x93, 0x02, handle_channel_state,      "Channel State"},

    // Lighting zones — reg_end capped to base + MAX_LIGHT_ZONES - 1 (= base + 3)
    {0xA0, 0xA3, 0x01, handle_light_zone_multicolor, "Light Zone Multicolor"},
    {0xB0, 0xB3, 0x01, handle_light_zone_name,    "Light Zone Name"},
    {0xC0, 0xC3, 0x01, handle_light_zone_state,   "Light Zone State"},
    {0xD0, 0xD3, 0x01, handle_light_zone_color,   "Light Zone Color"},
    {0xE0, 0xE3, 0x01, handle_light_zone_active,  "Light Zone Active"},

    // Valve labels (slot 0x02)
    {0xD0, 0xD1, 0x02, handle_valve_label,        "Valve Label"},

    // Favourite/mode enable flags (slot 0x03, registers 0x21-0x28 = Pool,Spa,Fav1-6)
    {0x21, 0x28, 0x03, handle_favourite_enable,       "Favourite Enable"},

    // Favourite/mode labels (slot 0x03, registers 0x31-0x38 = Pool,Spa,Fav1-6)
    {0x31, 0x38, 0x03, handle_favourite_label,        "Favourite Label"},

    // Heater 1 state (slot 0x00, register 0xE6) — touchscreen register-response broadcast.
    // The CMD 0x12 broadcast from 0x0062 is the authoritative state source that updates
    // pool_state->heaters[0]; this entry just names the register-response broadcast so it
    // stops being logged as "Unhandled register".
    {0xE6, 0xE6, 0x00, handle_heater1_state,          "Heater 1 State"},

    // Temperature setpoints (slot 0x00, registers 0xE7=Pool, 0xE8=Spa)
    {0xE7, 0xE8, 0x00, handle_temp_setpoint,          "Temperature Setpoint"},

    // Heater 2 state (slot 0x00, register 0xE9) — tentative; see PROTOCOL.md Appendix A note
    {0xE9, 0xE9, 0x00, handle_heater2_state,          "Heater 2 State"},

    {0xF4, 0xF4, 0x01, handle_channel_count,          "Channel Count"},
};

#define REGISTER_HANDLER_COUNT (sizeof(REGISTER_HANDLERS) / sizeof(REGISTER_HANDLERS[0]))

// Sensor disconnected / invalid reading sentinel — values >= 0xA0 (160°C) are
// not real water temperatures. Observed e.g. as 0xAF in a Connect 8/10 with
// no sensor wired up.
#define TEMP_INVALID_MIN 0xA0

static inline bool temp_is_invalid(uint8_t t) {
    return t >= TEMP_INVALID_MIN;
}

// Forward declaration — definition is further down with the other registry
// helpers, but handle_temp_reading() (below) needs to call it.
static int find_or_insert_seen_device_locked(pool_state_t *st, uint8_t hi, uint8_t lo);

/**
 * Handler: Water temperature reading (CMD 0x16 and CMD 0x31)
 *
 * Source-agnostic and dispatched on the CMD byte. The two CMDs carry the same
 * `{temp1, temp2}` field layout; the only practical difference is the
 * "sensor disconnected" encoding:
 *  - CMD 0x16: a disconnected sensor is reported as `0x00` (indistinguishable
 *    from a genuine 0°C reading — treated as valid by this handler).
 *  - CMD 0x31: a disconnected sensor is reported as `>= 0xA0` (a clean sentinel,
 *    e.g. 0xAF in installations with no sensor wired).
 *
 * Payload layouts (selected by LENGTH byte / payload_len):
 *  - LEN 0x0E (e.g. 0x0062 Connect 8/10): 2-byte payload `{temp1, temp2}` in °C.
 *  - LEN 0x0D (e.g. 0x0070/0x0072 Genus Heater family): 1-byte payload `{temp1}`.
 *
 * CMD 0x16 is the canonical source — it writes temp1/temp2 onto the source's
 * `seen_device_t` entry (looked up by the message's source address) and
 * publishes to MQTT. CMD 0x31 is log-only (the Connect 8/10 broadcasts both
 * ~70 ms apart with the same temp1; suppressing 0x31 avoids dual MQTT updates).
 * Values >= 0xA0 are skipped. `single_sensor_source` is committed at first
 * sight from the payload length so the MQTT topic shape stays stable.
 */
static bool handle_temp_reading(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    bool is_canonical = (data[7] == 0x16);
    const char *variant = is_canonical ? "" : " (alt)";

    uint8_t current_temp = payload[0];
    bool t1_invalid = temp_is_invalid(current_temp);

    if (payload_len >= 2) {
        uint8_t current_temp2 = payload[1];
        bool t2_invalid = temp_is_invalid(current_temp2);
        if (t1_invalid || t2_invalid) {
            char t1_buf[24], t2_buf[24];
            if (t1_invalid) snprintf(t1_buf, sizeof t1_buf, "INVALID (raw 0x%02X)", current_temp);
            else            snprintf(t1_buf, sizeof t1_buf, "%d°C", current_temp);
            if (t2_invalid) snprintf(t2_buf, sizeof t2_buf, "INVALID (raw 0x%02X)", current_temp2);
            else            snprintf(t2_buf, sizeof t2_buf, "%d°C", current_temp2);
            ESP_LOGW(TAG, "%s Current temperature%s - %s, temp2: %s",
                     addr_info, variant, t1_buf, t2_buf);
        } else {
            ESP_LOGI(TAG, "%s Current temperature%s - %d°C (temp2: %d°C)",
                     addr_info, variant, current_temp, current_temp2);
        }
    } else {
        if (t1_invalid) {
            ESP_LOGW(TAG, "%s Current temperature%s - INVALID (raw 0x%02X)",
                     addr_info, variant, current_temp);
        } else {
            ESP_LOGI(TAG, "%s Current temperature%s - %d°C", addr_info, variant, current_temp);
        }
    }

    if (!is_canonical) {
        return true;  // CMD 0x31 is log-only.
    }

    // Canonical path (CMD 0x16): write to the source device's temp slots and publish.
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for temp reading");
        return true;
    }

    int dev_idx = find_or_insert_seen_device_locked(ctx->pool_state, data[1], data[2]);
    if (dev_idx >= 0) {
        seen_device_t *dev = &ctx->pool_state->seen_devices[dev_idx];

        // Commit the source's sensor-count shape on first sight. Once set, it
        // stays — keeps MQTT topics stable across the device's lifetime.
        if (!dev->temp1_valid && !dev->temp2_valid) {
            dev->single_sensor_source = (payload_len < 2);
        }

        if (!t1_invalid) {
            dev->temp1 = current_temp;
            dev->temp1_valid = true;
        }
        if (payload_len >= 2 && !dev->single_sensor_source) {
            uint8_t t2 = payload[1];
            if (!temp_is_invalid(t2)) {
                dev->temp2 = t2;
                dev->temp2_valid = true;
            }
        }
    }

    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    bool publish_temp2 = (dev_idx >= 0)
                      && !ctx->pool_state->seen_devices[dev_idx].single_sensor_source
                      && ctx->pool_state->seen_devices[dev_idx].temp2_valid;
    xSemaphoreGive(ctx->state_mutex);

    if (t1_invalid || dev_idx < 0) {
        return true;  // Invalid temp1 or registry full: skip publish.
    }

    if (ctx->enable_mqtt) {
        mqtt_publish_temperature_reading(&snapshot, dev_idx, 1);
        if (publish_temp2) {
            mqtt_publish_temperature_reading(&snapshot, dev_idx, 2);
        }
    }

    return true;
}

/**
 * Handler: Temperature setpoint register messages
 * Register 0xE7 (Pool), 0xE8 (Spa), Slot 0x00
 */
static bool handle_temp_setpoint(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t temp_c = payload[2];
    bool is_pool = (reg_id == 0xE7);

    ESP_LOGI(TAG, "%s %s temperature setpoint - %d°C", addr_info,
             is_pool ? "Pool" : "Spa", temp_c);

    pool_state_t state_snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for temp setpoint");
        return true;
    }
    if (is_pool) {
        ctx->pool_state->pool_setpoint = temp_c;
    } else {
        ctx->pool_state->spa_setpoint = temp_c;
    }
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    state_snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_setpoints(&state_snapshot);
    }

    return true;
}

/**
 * Handler: Heater 1 state register-response broadcast
 * Register 0xE6, Slot 0x00. Log-only — authoritative state still flows through
 * the CMD 0x12 path (handle_heater) which updates pool_state->heaters[0].
 */
static bool handle_heater1_state(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t state = payload[2];
    ESP_LOGI(TAG, "%s Heater 1 state - %s (0x%02X)", addr_info,
             state == 0x00 ? "Off" : state == 0x01 ? "On" : "Unknown",
             state);

    return true;
}

/**
 * Handler: Heater 2 state — tentative
 * Register 0xE9, Slot 0x00. See PROTOCOL.md Appendix A.
 */
static bool handle_heater2_state(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t state = payload[2];
    ESP_LOGI(TAG, "%s Heater 2 state (tentative) - %s (0x%02X)", addr_info,
             state == 0x00 ? "Off" : state == 0x01 ? "On" : "Unknown",
             state);

    return true;
}

/**
 * Handler: Channel count
 * Register 0xF4, Slot 0x01
 * Reports the total number of channels configured in the system.
 */
static bool handle_channel_count(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t count = payload[2];
    if (count > MAX_CHANNELS) count = MAX_CHANNELS;

    ESP_LOGI(TAG, "%s Channel count - %d", addr_info, count);

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->num_channels = count;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return true;
}

/**
 * Handler: Genus Heater setpoints
 * Pattern: "02 00 70 FF FF 80 00 17 0E 15"
 *
 * Two-byte payload carrying both heater setpoints in °C.
 */
static bool handle_genus_heater_temp_setting(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t heater1_set = payload[0];
    uint8_t heater2_set = payload[1];

    ESP_LOGI(TAG, "%s Genus Heater setpoints - heater1=%d°C, heater2=%d°C",
             addr_info, heater1_set, heater2_set);
    return true;
}

/**
 * Handler: ICI Gas Heater device status
 * Pattern: "02 00 74 FF FF 80 00 12 10 16"
 *
 * Four-byte payload. All bytes are 0x00 when the heater is idle (modulation=0).
 * Full byte meanings when actively heating are not yet decoded.
 */
static bool handle_ici_heater_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 4) return false;
    ESP_LOGI(TAG, "%s ICI Gas Heater status - [%02X %02X %02X %02X]",
             addr_info, payload[0], payload[1], payload[2], payload[3]);
    return true;
}

/**
 * Handler: ICI Gas Heater setpoints
 * Pattern: "02 00 74 FF FF 80 00 17 0E 19"
 *
 * Two-byte payload — same frame format as the Genus Heater (0x0070) CMD 0x17 variant:
 * byte 10 = heat exchanger maximum temperature (°C), byte 11 = pool water target (°C).
 * Confirmed by observing byte 11 track button presses on the heater's local display.
 */
static bool handle_ici_heater_temp_setting(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t spa_setpoint  = payload[0];
    uint8_t pool_setpoint = payload[1];

    ESP_LOGI(TAG, "%s ICI Gas Heater setpoints - spa=%d°C, pool=%d°C",
             addr_info, spa_setpoint, pool_setpoint);

    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for ICI heater temp setting");
        return true;
    }
    ctx->pool_state->spa_setpoint   = spa_setpoint;
    ctx->pool_state->pool_setpoint  = pool_setpoint;
    ctx->pool_state->spa_setpoint_f  = spa_setpoint  * 9 / 5 + 32;
    ctx->pool_state->pool_setpoint_f = pool_setpoint * 9 / 5 + 32;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_setpoints(&snapshot);
    }

    return true;
}

/**
 * Handler: Heater status message
 * Pattern: "02 00 62 FF FF 80 00 12 0F"
 */
static bool handle_heater(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t heater_state = payload[1];
    ESP_LOGI(TAG, "%s Heater - %s", addr_info, heater_state ? "On" : "Off");

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for heater");
        return true;
    }
    ctx->pool_state->heaters[0].on = (heater_state != 0);
    ctx->pool_state->heaters[0].valid = true;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_heater(&snapshot, 0);
    }

    return true;
}

/**
 * Handler: Temperature setting message
 * Pattern: "02 00 50 FF FF 80 00 17 10 F7"
 */
static bool handle_temp_setting(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 4) return false;

    uint8_t spa_set_temp_c = payload[0];
    uint8_t pool_set_temp_c = payload[1];
    uint8_t spa_set_temp_f = payload[2];
    uint8_t pool_set_temp_f = payload[3];

    ESP_LOGI(TAG, "%s Temperature settings - spa=%d°C/%d°F, pool=%d°C/%d°F",
             addr_info, spa_set_temp_c, spa_set_temp_f, pool_set_temp_c, pool_set_temp_f);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for temp setting");
        return true;
    }
    ctx->pool_state->spa_setpoint = spa_set_temp_c;
    ctx->pool_state->pool_setpoint = pool_set_temp_c;
    ctx->pool_state->spa_setpoint_f = spa_set_temp_f;
    ctx->pool_state->pool_setpoint_f = pool_set_temp_f;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_setpoints(&snapshot);
    }

    return true;
}

/**
 * Handler: Mode message (Spa/Pool)
 * Pattern: "02 00 50 FF FF 80 00 14 0D F1"
 */
static bool handle_mode(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t mode = payload[0];
    const char *mode_str = (mode == 0x00) ? "Spa" : (mode == 0x01) ? "Pool" : "Unknown";
    ESP_LOGI(TAG, "%s Mode - %s", addr_info, mode_str);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for mode");
        return true;
    }
    ctx->pool_state->mode = mode;
    ctx->pool_state->mode_valid = true;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_mode(&snapshot);
    }

    return true;
}

/**
 * Handler: Configuration message
 * Pattern: "02 00 50 FF FF 80 00 26 0E 04"
 */
static bool handle_config(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t config_byte = payload[0];
    const char *scale_str        = (config_byte & 0x10) ? "Fahrenheit"   : "Celsius";
    const char *step_str         = (config_byte & 0x04) ? "2°"           : "1°";
    const char *heater_active_str = (config_byte & 0x08) ? "On"          : "Off";
    const char *mode_str         = (config_byte & 0x02) ? "cooler-only"  : "heat";
    ESP_LOGI(TAG, "%s Config - temperature scale=%s, step=%s, heater=%s, mode=%s",
             addr_info, scale_str, step_str, heater_active_str, mode_str);

    // Update state only (no MQTT publishing)
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->temp_scale_fahrenheit = (config_byte & 0x10) != 0;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return false;  // Intentionally return false to match original behavior
}

/**
 * Handler: Controller time/clock message
 * Pattern: "02 00 50 FF FF 80 00 FD 0F DC"
 */
static bool handle_controller_time(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t minutes = payload[0];
    uint8_t hours = payload[1];
    uint8_t day_of_week = payload[2];  // 0=Monday, 6=Sunday

    const char *day_name = (day_of_week < DAY_OF_WEEK_COUNT) ? DAY_OF_WEEK_NAMES[day_of_week] : "Unknown";
    ESP_LOGI(TAG, "%s Controller time - %02d:%02d %s", addr_info, hours, minutes, day_name);

    // Update state only (no MQTT publishing)
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->controller_minutes = minutes;
        ctx->pool_state->controller_hours = hours;
        ctx->pool_state->controller_day_of_week = day_of_week;
        ctx->pool_state->controller_time_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return true;
}

// Find or insert a seen-device entry. Returns slot index, or -1 if full.
// Caller must hold the pool-state mutex.
static int find_or_insert_seen_device_locked(pool_state_t *st, uint8_t hi, uint8_t lo)
{
    for (int i = 0; i < st->num_seen_devices; i++) {
        if (st->seen_devices[i].addr_hi == hi && st->seen_devices[i].addr_lo == lo) {
            return i;
        }
    }
    if (st->num_seen_devices >= MAX_SEEN_DEVICES) return -1;
    int idx = st->num_seen_devices++;
    st->seen_devices[idx].addr_hi = hi;
    st->seen_devices[idx].addr_lo = lo;
    st->seen_devices[idx].fw_version_valid = false;
    st->seen_devices[idx].fw_version_major = 0;
    st->seen_devices[idx].fw_version_minor = 0;
    st->seen_devices[idx].decoded_count = 0;
    st->seen_devices[idx].unknown_count = 0;
    st->seen_devices[idx].temp1 = 0;
    st->seen_devices[idx].temp2 = 0;
    st->seen_devices[idx].temp1_valid = false;
    st->seen_devices[idx].temp2_valid = false;
    st->seen_devices[idx].single_sensor_source = false;
    return idx;
}

/**
 * Handler: Firmware version (CMD 0x0A, source-agnostic)
 *
 * Covers PROTOCOL.md command `0x0A` (consolidated). Same 2-byte `{major, minor}`
 * payload shape across every observed source — the source address selects
 * which `pool_state->*_version_*` field is populated.
 *
 * Unknown sources are logged but skipped for state-update purposes.
 */
static bool handle_firmware_version(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t major = payload[0];
    uint8_t minor = payload[1];
    uint16_t src = ((uint16_t)data[1] << 8) | data[2];

    ESP_LOGI(TAG, "%s Firmware version - %d.%d", addr_info, major, minor);

    if (!ctx->state_mutex) return true;
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return true;

    switch (src) {
        case 0x0050:
            ctx->pool_state->touchscreen_version_major = major;
            ctx->pool_state->touchscreen_version_minor = minor;
            ctx->pool_state->touchscreen_version_valid = true;
            break;
        case 0x0062:
            ctx->pool_state->controller_version_major = major;
            ctx->pool_state->controller_version_minor = minor;
            ctx->pool_state->controller_version_valid = true;
            break;
        case 0x0084:
            ctx->pool_state->chlor_version_major = major;
            ctx->pool_state->chlor_version_minor = minor;
            ctx->pool_state->chlor_version_valid = true;
            break;
        case 0x00F0:
            ctx->pool_state->gateway_version_major = major;
            ctx->pool_state->gateway_version_minor = minor;
            ctx->pool_state->gateway_version_valid = true;
            break;
        // 0x0070 (Genus Heater) and any future device — log-only, no dedicated state field yet
    }

    int dev_idx = find_or_insert_seen_device_locked(ctx->pool_state, data[1], data[2]);
    if (dev_idx >= 0) {
        ctx->pool_state->seen_devices[dev_idx].fw_version_valid = true;
        ctx->pool_state->seen_devices[dev_idx].fw_version_major = major;
        ctx->pool_state->seen_devices[dev_idx].fw_version_minor = minor;
    }

    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    xSemaphoreGive(ctx->state_mutex);

    return true;
}

/**
 * Handler: Touchscreen other status info message
 * Pattern: "02 00 50 FF FF 80 00 12 0E F0"
 */
static bool handle_touchscreen_unknown1(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t data_byte1 = payload[0];
    uint8_t data_byte2 = payload[1];

    if (data_byte1 != 0x05 || data_byte2 != 0x00) {
        ESP_LOGW(TAG, "%s Touchscreen other status - UNEXPECTED VALUE: Byte1: 0x%02X (%d), Byte2: 0x%02X (%d) (expected 0x05 0x00)",
                 addr_info, data_byte1, data_byte1, data_byte2, data_byte2);
    } else {
        ESP_LOGI(TAG, "%s Touchscreen other status - Byte1: 0x%02X (%d), Byte2: 0x%02X (%d)",
                 addr_info, data_byte1, data_byte1, data_byte2, data_byte2);
    }

    return true;
}

/**
 * Handler: Unknown/unhandled message
 *
 * Logs everything we *can* identify about an unknown message so it can be
 * triaged from logs without a full hex re-read:
 *   - addr_info: source/destination, resolved to device names when known
 *   - CMD byte (data[7]): the protocol command byte
 *   - LEN byte (data[8]): the frame's declared length
 *   - payload bytes only (between header and frame checksum)
 *
 * The full raw frame is already emitted as "RX MSG" by decode_message()
 * before dispatch, so it's not repeated here.
 */
static bool handle_unknown(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    // Format payload bytes as hex string (data section only)
    char payload_hex[3 * BUS_MESSAGE_MAX_SIZE + 1];
    int pos = 0;
    for (int i = 0; i < payload_len && pos < (int)sizeof(payload_hex) - 3; i++) {
        pos += snprintf(&payload_hex[pos], sizeof(payload_hex) - pos, "%02X ", payload[i]);
    }
    // Strip trailing space if any payload was written
    if (pos > 0) payload_hex[pos - 1] = '\0';
    else payload_hex[0] = '\0';

    uint8_t cmd = data[7];
    uint8_t length_byte = data[8];

    char cmd_name_buf[24];
    const char *cmd_name = get_cmd_name(cmd, cmd_name_buf, sizeof(cmd_name_buf));

    ESP_LOGW(TAG, "Unhandled %s CMD=0x%02X (%s) LEN=%u payload=[%s]",
             addr_info, cmd, cmd_name, length_byte, payload_hex);

    return false;  // Not decoded
}

/**
 * Handler: Controller status message
 * Pattern: "02 00 50 FF FF 80 00 27 0D 04"
 */
static bool handle_touchscreen_unknown2(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    // Short form of valve state broadcast (startup / no valve state available)
    ESP_LOGI(TAG, "%s Valve state broadcast (startup form)", addr_info);
    return true;
}

/**
 * Handler: Touchscreen unknown broadcast (CMD 0x05)
 * Invariant across all captures: data byte always 0x01.
 * Silenced here to avoid spurious "Unhandled" warnings.
 */
static bool handle_touchscreen_unknown3(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    ESP_LOGI(TAG, "%s Touchscreen unknown (CMD 0x05): 0x%02X", addr_info,
             payload_len > 0 ? payload[0] : 0);
    return true;
}

/**
 * Handler: Valve state broadcast (long form)
 * Pattern: "02 00 50 FF FF 80 00 27 13 0A"
 * Byte 10: slot count; then 3 bytes per slot: [configured][state][active]
 */
static bool handle_valve_state(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t slot_count = payload[0];
    if (slot_count > MAX_VALVE_SLOTS) slot_count = MAX_VALVE_SLOTS;

    if (payload_len < 1 + slot_count * 3) {
        ESP_LOGW(TAG, "%s Valve state - truncated payload (slots=%d, payload_len=%d)",
                 addr_info, slot_count, payload_len);
        return true;
    }

    // Log each configured valve before taking the mutex
    for (int i = 0; i < slot_count; i++) {
        bool configured = (payload[1 + i * 3] == 0x01);
        uint8_t state   = payload[2 + i * 3];
        bool active     = (payload[3 + i * 3] == 0x01);
        if (configured) {
            const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";
            ESP_LOGI(TAG, "%s Valve %d - %s (%s)", addr_info, i + 1,
                     state_name, active ? "Active" : "Inactive");
        }
    }

    bool changed = false;
    bool new_valve_configured = false;
    pool_state_t state_snapshot;
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->num_valve_slots = slot_count;
        for (int i = 0; i < slot_count; i++) {
            bool configured = (payload[1 + i * 3] == 0x01);
            uint8_t state   = payload[2 + i * 3];
            bool active     = (payload[3 + i * 3] == 0x01);
            valve_state_t *v = &ctx->pool_state->valves[i];
            if (!v->configured && configured) {
                new_valve_configured = true;
            }
            if (v->configured != configured || v->state != state || v->active != active) {
                v->configured = configured;
                v->state      = state;
                v->active     = active;
                changed = true;
            }
        }
        if (changed) {
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        }
        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    // Wake the register requester to fetch the label for any newly seen valve
    if (new_valve_configured) {
        register_requester_notify();
    }

    if (changed && ctx->enable_mqtt) {
        for (int i = 0; i < slot_count; i++) {
            mqtt_publish_valve(&state_snapshot, i + 1);
        }
    }

    return true;
}

/**
 * Handler: Internet Gateway serial number message
 * Pattern: "02 00 F0 FF FF 80 00 37 11 B8"
 */
static bool handle_serial_number(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 5) return false;

    // Serial number is in payload[1-4] (little endian)
    uint32_t serial = UINT32_LE(payload, 1);
    ESP_LOGI(TAG, "%s Serial number - %" PRIu32 " (0x%08" PRIX32 ")", addr_info, serial, serial);

    // Update state only (no MQTT publishing)
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->serial_number = serial;
        ctx->pool_state->serial_number_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return true;
}

/**
 * Handler: Internet Gateway IP address message
 * Pattern: "02 00 F0 FF FF 80 00 37 15 BC"
 */
static bool handle_gateway_ip(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 9) return false;

    // IP address is in payload[4-7], signal level at payload[8]
    uint8_t ip[4];
    ip[0] = payload[4];
    ip[1] = payload[5];
    ip[2] = payload[6];
    ip[3] = payload[7];
    uint8_t signal_level = payload[8];

    ESP_LOGI(TAG, "%s Internet Gateway IP - %d.%d.%d.%d, signal level: %d",
             addr_info, ip[0], ip[1], ip[2], ip[3], signal_level);

    // Update state only (no MQTT publishing)
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        memcpy(ctx->pool_state->gateway_ip, ip, 4);
        ctx->pool_state->gateway_signal_level = signal_level;
        ctx->pool_state->gateway_ip_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return true;
}

/**
 * Handler: Internet Gateway communications status message
 * Pattern: "02 00 F0 FF FF 80 00 37 0F B6"
 */
static bool handle_gateway_comms(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    // Comms status is in payload[1-2] (little endian)
    uint16_t comms_status = UINT16_LE(payload, 1);
    const char *status_text = get_gateway_comms_status_text(comms_status);

    ESP_LOGI(TAG, "%s Internet Gateway comms status - %u (%s)", addr_info, comms_status, status_text);

    // Update state only (no MQTT publishing)
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->gateway_comms_status = comms_status;
        ctx->pool_state->gateway_comms_status_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return true;
}

/**
 * Handler: Internet Gateway status broadcast
 * Pattern: "02 00 F0 FF FF 80 00 12 0F 91"
 *
 * Payload (3 bytes): { major, minor, embedded_checksum }
 * where embedded_checksum == major + minor. The frame checksum at byte 13
 * is handled by the framing layer and is not part of the payload.
 *
 * Firmware-version state is populated by the generic CMD 0x0A handler
 * (handle_firmware_version), so this handler is log-only.
 */
static bool handle_gateway_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t major = payload[0];
    uint8_t minor = payload[1];
    uint8_t embedded_checksum = payload[2];
    uint8_t expected_checksum = (uint8_t)(major + minor);

    if (embedded_checksum == expected_checksum) {
        ESP_LOGI(TAG, "%s Internet Gateway status - firmware %d.%d (checksum 0x%02X OK)",
                 addr_info, major, minor, embedded_checksum);
    } else {
        ESP_LOGW(TAG, "%s Internet Gateway status - firmware %d.%d (checksum 0x%02X, expected 0x%02X)",
                 addr_info, major, minor, embedded_checksum, expected_checksum);
    }

    return true;
}

/**
 * Handler: Register read request (CMD 0x39) — source-agnostic.
 *
 * Observed from both the Internet Gateway (`0x00F0`) and the Genus Heater
 * (`0x0070`); payload shape `{reg_id, slot_id}` is identical, only the source
 * address (and resulting checksum1 byte) differs. Dispatched in
 * dispatch_message() by CMD byte.
 */
static bool handle_register_read_request(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t reg_id = payload[0];
    uint8_t slot_id = payload[1];

    // Resolve a human-readable description by searching the register dispatch table
    char desc[48];
    bool found = false;
    for (int i = 0; i < REGISTER_HANDLER_COUNT; i++) {
        const register_handler_t *entry = &REGISTER_HANDLERS[i];
        if (reg_id >= entry->reg_start && reg_id <= entry->reg_end && entry->slot == slot_id) {
            snprintf(desc, sizeof(desc), "%s %d", entry->name, reg_id - entry->reg_start + 1);
            found = true;
            break;
        }
    }
    if (!found) {
        snprintf(desc, sizeof(desc), "0x%02X/0x%02X", reg_id, slot_id);
    }

    ESP_LOGI(TAG, "%s Register read request - %s", addr_info, desc);

    // No state update needed - this is just a request message
    return true;
}

/**
 * Handler: Channel toggle command (Gateway -> Controller)
 * Pattern: "02 00 F0 FF FF 80 00 10 0D 8D"
 */
static bool handle_channel_toggle_cmd(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t channel_idx = payload[0];
    uint8_t channel_num = channel_idx + 1;  // Convert to 1-based

    // Look up channel name from pool state
    char channel_name[32] = {0};
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        if (channel_idx < MAX_CHANNELS && ctx->pool_state->channels[channel_idx].configured) {
            strncpy(channel_name, ctx->pool_state->channels[channel_idx].name, sizeof(channel_name) - 1);
        }
        xSemaphoreGive(ctx->state_mutex);
    }

    if (channel_name[0] != '\0') {
        ESP_LOGI(TAG, "%s Gateway channel toggle command - Channel %d (%s)",
                 addr_info, channel_num, channel_name);
    } else {
        ESP_LOGI(TAG, "%s Gateway channel toggle command - Channel %d (index 0x%02X, name unknown)",
                 addr_info, channel_num, channel_idx);
    }

    // No state update needed - this is a command message, not status
    // The controller will respond with an updated Channel Status message
    return true;
}

/**
 * Handler: Temperature setpoint command — Pool/Spa target (CMD 0x19, slot 0x01/0x02) — source-agnostic.
 * Used by the Internet Gateway (0x00F0) to set the Pool or Spa setpoint; the
 * temperature is repeated at payload[1] and payload[2]. The controller
 * responds with an updated Temperature Settings (CMD 0x17).
 * Dispatched in dispatch_message() by CMD byte.
 */
static bool handle_temp_set_cmd_pool_spa(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t target = payload[0];
    uint8_t temp_c = payload[1];  // Repeated at payload[2], only need one

    const char *target_name = (target == 0x01) ? "Pool" : "Spa";
    ESP_LOGI(TAG, "%s Temperature set command - %s setpoint -> %d°C",
             addr_info, target_name, temp_c);

    // No state update needed - the controller will broadcast the new setpoint
    return true;
}

/**
 * Handler: Temperature setpoint command — heater pair target (CMD 0x19, slot 0x03) — source-agnostic.
 * Used by the Touch Screen (0x0050) writing to the internal heater-setpoints
 * address 0x007F: 5-byte payload carrying both heater setpoints in °C and °F.
 *   payload[0] = 0x03  (heater-pair slot marker)
 *   payload[1] = Heater 2 setpoint °C
 *   payload[2] = Heater 1 setpoint °C
 *   payload[3] = Heater 2 setpoint °F
 *   payload[4] = Heater 1 setpoint °F
 * Byte order is reversed vs the 0x17 broadcast from 0x0070 (which is [H1, H2]).
 * The heater (0x0070) responds with an updated Genus Heater Temperature
 * Setting (CMD 0x17). Dispatched in dispatch_message() by CMD byte.
 */
static bool handle_temp_set_cmd_heaters(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 5) return false;

    uint8_t h2_c = payload[1];
    uint8_t h1_c = payload[2];
    uint8_t h2_f = payload[3];
    uint8_t h1_f = payload[4];

    ESP_LOGI(TAG, "%s Heater setpoint command - heater1=%d°C/%d°F, heater2=%d°C/%d°F",
             addr_info, h1_c, h1_f, h2_c, h2_f);

    // No state update needed - the heater will broadcast the new setpoints
    return true;
}

/**
 * Handler: Light zone control command (Gateway -> Controller)
 * Pattern: "02 00 F0 FF FF 80 00 3A 0F B9"
 */
static bool handle_light_control_cmd(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t slot = payload[1];
    uint8_t state = payload[2];

    // Dispatch based on register ID and slot
    if (reg_id >= 0xC0 && reg_id <= 0xC7 && slot == 0x01) {
        // Light zone state control (0xC0-0xC7, slot 0x01)
        uint8_t zone_num = reg_id - 0xC0 + 1;
        const char *state_name = (state == 0x00) ? "Off" : (state == 0x01) ? "Auto" : (state == 0x02) ? "On" : "Unknown";
        ESP_LOGI(TAG, "%s Gateway light control command - Zone %d -> %s (0x%02X)",
                 addr_info, zone_num, state_name, state);
    } else if (reg_id == 0xE6 && slot == 0x00) {
        // Heater on/off control (0xE6, slot 0x00)
        ESP_LOGI(TAG, "%s Gateway heater control command - Heater -> %s",
                 addr_info, state ? "On" : "Off");
    } else {
        ESP_LOGW(TAG, "%s Gateway register write command - Unknown Reg=0x%02X, Slot=0x%02X, State=0x%02X",
                 addr_info, reg_id, slot, state);
    }

    // No state update needed - this is a command message, not status
    // The controller will respond with a register status update
    return true;
}

/**
 * Handler: Mode control command (Gateway -> Controller)
 * Pattern: "02 00 F0 00 50 80 00 2A 0D F9"
 */
static bool handle_mode_control_cmd(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t mode_value = payload[0];
    const char *mode_name;
    if (mode_value == 0x00)      mode_name = "Pool";
    else if (mode_value == 0x01) mode_name = "Spa";
    else if (mode_value == 0x80) mode_name = "All Off";
    else if (mode_value == 0x81) mode_name = "All Auto";
    else if (mode_value >= 0x02 && mode_value <= 0x07) mode_name = "Favourite";
    else mode_name = "Unknown";

    ESP_LOGI(TAG, "%s Gateway mode control command - %s (0x%02X)",
             addr_info, mode_name, mode_value);

    pool_state_t state_snapshot;
    bool should_publish = false;

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->active_favourite = mode_value;
        ctx->pool_state->active_favourite_valid = true;
        state_snapshot = *ctx->pool_state;
        should_publish = true;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (should_publish) {
        mqtt_publish_favourite(&state_snapshot);
    }
    return true;
}

/**
 * Handler: Chlorinator pH setpoint (CMD 0x1D, channel 0x01) — source-agnostic.
 * Same `{channel, value_lo, value_hi}` payload from both 0x0090 RolaChem and
 * 0x0084 Viron chlorinators; dispatched in dispatch_message() by CMD byte.
 */
static bool handle_chlor_ph_setpoint(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint16_t value = UINT16_LE(payload, 1);
    ESP_LOGI(TAG, "%s Chlorinator pH setpoint - %.1f", addr_info, value / 10.0);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for pH setpoint");
        return true;
    }
    ctx->pool_state->ph_setpoint = value;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}

/**
 * Handler: Chlorinator ORP setpoint (CMD 0x1D, channel 0x02) — source-agnostic.
 * Same `{channel, value_lo, value_hi}` payload from both 0x0090 RolaChem and
 * 0x0084 Viron chlorinators; dispatched in dispatch_message() by CMD byte.
 */
static bool handle_chlor_orp_setpoint(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint16_t value = UINT16_LE(payload, 1);
    ESP_LOGI(TAG, "%s Chlorinator ORP setpoint - %d mV", addr_info, value);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for ORP setpoint");
        return true;
    }
    ctx->pool_state->orp_setpoint = value;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}

/**
 * Handler: Chlorinator pH reading (CMD 0x1F, channel 0x01) — source-agnostic.
 * Same `{channel, value_lo, value_hi}` payload from both 0x0090 RolaChem and
 * 0x0084 Viron chlorinators; dispatched in dispatch_message() by CMD byte.
 */
static bool handle_chlor_ph_reading(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint16_t value = UINT16_LE(payload, 1);
    ESP_LOGI(TAG, "%s Chlorinator pH reading - %.1f", addr_info, value / 10.0);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for pH reading");
        return true;
    }
    ctx->pool_state->ph_reading = value;
    ctx->pool_state->ph_valid = true;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}

/**
 * Handler: Chlorinator ORP reading (CMD 0x1F, channel 0x02) — source-agnostic.
 * Same `{channel, value_lo, value_hi}` payload from both 0x0090 RolaChem and
 * 0x0084 Viron chlorinators; dispatched in dispatch_message() by CMD byte.
 */
static bool handle_chlor_orp_reading(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint16_t value = UINT16_LE(payload, 1);
    ESP_LOGI(TAG, "%s Chlorinator ORP reading - %d mV", addr_info, value);

    // Update state and publish
    pool_state_t snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for ORP reading");
        return true;
    }
    ctx->pool_state->orp_reading = value;
    ctx->pool_state->orp_valid = true;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    if (ctx->enable_mqtt) {
        mqtt_publish_chlorinator(&snapshot);
    }

    return true;
}

/**
 * Handler: Chlorinator status broadcast (PROTOCOL.md §32)
 * Patterns: "02 00 90 FF FF 80 00 12 0D 2F" (variant A)
 *           "02 00 84 FF FF 80 00 12 0D 23" (variant B)
 *
 * 1-byte payload: configured operating mode. Mapping is tentative — see §32.
 */
static bool handle_chlor_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t mode = payload[0];
    const char *mode_name;
    switch (mode) {
        case 0x00: mode_name = "Off";  break;
        case 0x01: mode_name = "Auto"; break;
        case 0x02: mode_name = "On";   break;
        default:   mode_name = "Unknown"; break;
    }

    ESP_LOGI(TAG, "%s Chlorinator status - mode=%s (0x%02X)", addr_info, mode_name, mode);

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->chlor_mode = mode;
        ctx->pool_state->chlor_mode_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xSemaphoreGive(ctx->state_mutex);
    }

    return true;
}

/**
 * Handler: Light configuration message
 * Pattern: "02 00 50 FF FF 80 00 06 0E E4"
 */
static bool handle_light_config(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 2) return false;

    uint8_t zone_idx  = payload[0];
    uint8_t light_on  = payload[1];

    if (zone_idx <= 3) {
        ESP_LOGI(TAG, "%s Lighting zone %d - %s", addr_info, zone_idx + 1, light_on ? "On" : "Off");

        pool_state_t state_snapshot;
        bool newly_configured = false;

        if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to acquire mutex for light config");
            return true;
        }
        newly_configured = !ctx->pool_state->lighting[zone_idx].configured;
        ctx->pool_state->lighting[zone_idx].zone       = zone_idx + 1;
        ctx->pool_state->lighting[zone_idx].configured = true;
        ctx->pool_state->lighting[zone_idx].active     = (light_on != 0);
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);

        if (newly_configured) {
            register_requester_notify();
        }

        if (ctx->enable_mqtt) {
            mqtt_publish_light(&state_snapshot, zone_idx + 1);
        }
    }

    return true;
}

// ======================================================
// Register message handlers (dispatched by register range and slot)
// ======================================================

/**
 * Handler: Timer configuration
 * Register range: 0x08-0x17 (timers 1-16), Slot: 0x04
 *
 * Payload layout (bytes within payload[], offset from byte 10):
 *   [0] reg_id       - register (0x08=timer1 .. 0x17=timer16)
 *   [1] slot         - always 0x04
 *   [2] start_hour   - 24h start hour
 *   [3] start_minute - start minute
 *   [4] stop_hour    - 24h stop hour
 *   [5] stop_minute  - stop minute
 *   [6] days         - bitmask (assumed: bit0=Mon..bit6=Sun; 0x7F=every day, 0x00=disabled)
 */
static bool handle_timer(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 7) return false;

    uint8_t reg_id       = payload[0];
    // payload[1] = slot (0x04) - not needed
    uint8_t start_hour   = payload[2];
    uint8_t start_minute = payload[3];
    uint8_t stop_hour    = payload[4];
    uint8_t stop_minute  = payload[5];
    uint8_t days         = payload[6];

    uint8_t timer_num = reg_id - 0x08 + 1;

    // Build compact day string: MTWTFSS where '-' means not set
    // Assumed mapping: bit0=Mon, bit1=Tue, bit2=Wed, bit3=Thu, bit4=Fri, bit5=Sat, bit6=Sun
    char days_str[8];
    const char day_chars[] = "MTWTFSS";
    for (int i = 0; i < 7; i++) {
        days_str[i] = (days & (1 << i)) ? day_chars[i] : '-';
    }
    days_str[7] = '\0';

    if (days == 0x00 && start_hour == 0 && start_minute == 0 && stop_hour == 0 && stop_minute == 0) {
        ESP_LOGI(TAG, "%s Timer %d - not configured", addr_info, timer_num);
    } else {
        ESP_LOGI(TAG, "%s Timer %d - start=%02d:%02d stop=%02d:%02d days=0x%02X [%s]",
                 addr_info, timer_num,
                 start_hour, start_minute, stop_hour, stop_minute,
                 days, days_str);
    }

    // Log any extra bytes beyond the 7 known bytes (for future decoding)
    if (payload_len > 7) {
        int extra = payload_len - 7;
        char extra_hex[64] = {0};
        int pos = 0;
        for (int i = 7; i < payload_len && pos < (int)sizeof(extra_hex) - 4; i++) {
            pos += snprintf(&extra_hex[pos], sizeof(extra_hex) - pos, "%02X ", payload[i]);
        }
        ESP_LOGW(TAG, "%s Timer %d - %d extra unknown byte(s): %s", addr_info, timer_num, extra, extra_hex);
    }

    // Update state
    if (timer_num >= 1 && timer_num <= MAX_TIMERS) {
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            int idx = timer_num - 1;
            ctx->pool_state->timers[idx].timer_num    = timer_num;
            ctx->pool_state->timers[idx].start_hour   = start_hour;
            ctx->pool_state->timers[idx].start_minute = start_minute;
            ctx->pool_state->timers[idx].stop_hour    = stop_hour;
            ctx->pool_state->timers[idx].stop_minute  = stop_minute;
            ctx->pool_state->timers[idx].days         = days;
            ctx->pool_state->timers[idx].valid        = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(ctx->state_mutex);
        }
    }

    return true;
}

/**
 * Handler: Channel type configuration
 * Register range: 0x6C-0x73, Slot: 0x02
 */
static bool handle_channel_type(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t ch_type = payload[2];
    uint8_t ch_num = reg_id - 0x6C + 1;

    const char *type_name = get_channel_type_name(ch_type);
    ESP_LOGI(TAG, "%s Channel %d type - %s (%d)", addr_info, ch_num, type_name, ch_type);

    if (ch_type != CHANNEL_UNUSED) {
        // Update pool state
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            ctx->pool_state->channels[ch_num - 1].type = ch_type;
            ctx->pool_state->channels[ch_num - 1].configured = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            xSemaphoreGive(ctx->state_mutex);
        }
    }

    return true;
}

/**
 * Handler: Channel names
 * Register range: 0x7C-0x83, Slot: 0x02
 */
static bool handle_channel_name(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    // Need at least 3 bytes: register ID, slot, and name data (even if null terminator)
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t ch_num = reg_id - 0x7C + 1;

    // Safely copy string from payload — protocol does not guarantee null termination
    char name[32] = {0};
    int str_len = payload_len - 2;
    if (str_len > (int)sizeof(name) - 1) str_len = (int)sizeof(name) - 1;
    memcpy(name, &payload[2], str_len);

    // Check if it's an empty/unused channel (first byte is 0x00)
    if (name[0] == '\0') {
        ESP_LOGI(TAG, "%s Channel %d name - (empty)", addr_info, ch_num);
    } else {
        ESP_LOGI(TAG, "%s Channel %d name - \"%s\"", addr_info, ch_num, name);

        // Update pool state
        if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            if (ch_num <= MAX_CHANNELS) {
                strncpy(ctx->pool_state->channels[ch_num - 1].name, name, sizeof(ctx->pool_state->channels[ch_num - 1].name) - 1);
                ctx->pool_state->channels[ch_num - 1].id = ch_num;
                ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            }
            xSemaphoreGive(ctx->state_mutex);
        }
    }

    return true;
}

/**
 * Handler: Channel state (read-only broadcast)
 * Register range: 0x8C-0x93, Slot: 0x02
 * Values: 0x00=Off, 0x01=Auto, 0x02=On
 * Note: write commands (0x3A) targeting these registers are silently ignored by the controller.
 *       Use the Channel Toggle Command to change channel state.
 */
static bool handle_channel_state(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t state  = payload[2];
    uint8_t ch_num = reg_id - 0x8C + 1;

    const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";
    ESP_LOGI(TAG, "%s Channel %d state - %s", addr_info, ch_num, state_name);

    if (ch_num > MAX_CHANNELS) return true;

    pool_state_t state_snapshot;
    bool changed = false;
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        channel_state_t *ch = &ctx->pool_state->channels[ch_num - 1];
        if (!ch->configured || ch->state != state) {
            ch->state = state;
            ch->configured = true;
            ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            changed = true;
        }
        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (changed && ctx->enable_mqtt) {
        mqtt_publish_channel(&state_snapshot, ch_num);
    }

    return true;
}

/**
 * Handler: Lighting zone state
 * Register range: 0xC0-0xC7, Slot: 0x01
 */
static bool handle_light_zone_state(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t state = payload[2];
    uint8_t zone_idx = reg_id - 0xC0;

    if (zone_idx >= MAX_LIGHT_ZONES) {
        ESP_LOGW(TAG, "%s Lighting zone state: zone_idx %d out of range (max %d)", addr_info, zone_idx, MAX_LIGHT_ZONES);
        return false;
    }

    const char *state_name = (state < LIGHTING_STATE_COUNT) ? LIGHTING_STATE_NAMES[state] : "Unknown";
    ESP_LOGI(TAG, "%s Lighting zone %d state - %s", addr_info, zone_idx + 1, state_name);

    bool should_publish = false;
    uint8_t zone_num = 0;
    pool_state_t state_snapshot;

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->lighting[zone_idx].zone = zone_idx + 1;
        ctx->pool_state->lighting[zone_idx].state = state;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (ctx->pool_state->lighting[zone_idx].configured) {
            should_publish = true;
            zone_num = ctx->pool_state->lighting[zone_idx].zone;
        }

        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (should_publish && ctx->enable_mqtt) {
        mqtt_publish_light(&state_snapshot, zone_num);
    }

    return true;
}

/**
 * Handler: Lighting zone color
 * Register range: 0xD0-0xD7, Slot: 0x01
 */
static bool handle_light_zone_color(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t color = payload[2];
    uint8_t zone_idx = reg_id - 0xD0;

    if (zone_idx >= MAX_LIGHT_ZONES) {
        ESP_LOGW(TAG, "%s Lighting zone color: zone_idx %d out of range (max %d)", addr_info, zone_idx, MAX_LIGHT_ZONES);
        return false;
    }

    const char *color_name = (color < LIGHTING_COLOR_COUNT) ? LIGHTING_COLOR_NAMES[color] : "Unknown";
    ESP_LOGI(TAG, "%s Lighting zone %d color - %s (%d)", addr_info, zone_idx + 1, color_name, color);

    bool should_publish = false;
    uint8_t zone_num = 0;
    pool_state_t state_snapshot;

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->lighting[zone_idx].color = color;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (ctx->pool_state->lighting[zone_idx].configured) {
            should_publish = true;
            zone_num = ctx->pool_state->lighting[zone_idx].zone;
        }

        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (should_publish && ctx->enable_mqtt) {
        mqtt_publish_light(&state_snapshot, zone_num);
    }

    return true;
}

/**
 * Handler: Lighting zone multicolor capability
 * Register range: 0xA0-0xA7, Slot: 0x01
 */
static bool handle_light_zone_multicolor(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t capable = payload[2];
    uint8_t zone_idx = reg_id - 0xA0;

    if (zone_idx >= MAX_LIGHT_ZONES) {
        ESP_LOGW(TAG, "%s Lighting zone multicolor: zone_idx %d out of range (max %d)", addr_info, zone_idx, MAX_LIGHT_ZONES);
        return false;
    }

    ESP_LOGI(TAG, "%s Lighting zone %d multicolor - %s", addr_info, zone_idx + 1, capable ? "Yes" : "No");

    pool_state_t state_snapshot;
    bool should_publish = false;

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->lighting[zone_idx].multicolor = (capable != 0);
        ctx->pool_state->lighting[zone_idx].multicolor_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        state_snapshot = *ctx->pool_state;
        should_publish = ctx->pool_state->lighting[zone_idx].configured;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (ctx->enable_mqtt && should_publish) {
        mqtt_publish_light(&state_snapshot, zone_idx + 1);
    }

    return true;
}

/**
 * Handler: Lighting zone preset name
 * Register range: 0xB0-0xB7, Slot: 0x01
 */
static bool handle_light_zone_name(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t name_id = payload[2];
    uint8_t zone_idx = reg_id - 0xB0;

    if (zone_idx >= MAX_LIGHT_ZONES) {
        ESP_LOGW(TAG, "%s Lighting zone name: zone_idx %d out of range (max %d)", addr_info, zone_idx, MAX_LIGHT_ZONES);
        return false;
    }

    const char *name = (name_id < LIGHT_ZONE_NAME_COUNT) ? LIGHT_ZONE_NAME_TABLE[name_id] : "Unknown";
    ESP_LOGI(TAG, "%s Lighting zone %d name - %s (%d)", addr_info, zone_idx + 1, name, name_id);

    pool_state_t state_snapshot;
    bool should_publish = false;

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->lighting[zone_idx].name_id = name_id;
        ctx->pool_state->lighting[zone_idx].name_valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        state_snapshot = *ctx->pool_state;
        should_publish = ctx->pool_state->lighting[zone_idx].configured;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (ctx->enable_mqtt && should_publish) {
        mqtt_publish_light(&state_snapshot, zone_idx + 1);
    }

    return true;
}

/**
 * Handler: Lighting zone active state
 * Register range: 0xE0-0xE7, Slot: 0x01
 */
static bool handle_light_zone_active(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t active = payload[2];
    uint8_t zone_idx = reg_id - 0xE0;

    if (zone_idx >= MAX_LIGHT_ZONES) {
        ESP_LOGW(TAG, "%s Lighting zone active: zone_idx %d out of range (max %d)", addr_info, zone_idx, MAX_LIGHT_ZONES);
        return false;
    }

    ESP_LOGI(TAG, "%s Lighting zone %d active - %s", addr_info, zone_idx + 1, active ? "Yes" : "No");

    bool should_publish = false;
    uint8_t zone_num = 0;
    pool_state_t state_snapshot;

    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->lighting[zone_idx].active = (active != 0);
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (ctx->pool_state->lighting[zone_idx].configured) {
            should_publish = true;
            zone_num = ctx->pool_state->lighting[zone_idx].zone;
        }

        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    if (should_publish && ctx->enable_mqtt) {
        mqtt_publish_light(&state_snapshot, zone_num);
    }

    return true;
}

/**
 * Handler: Valve labels
 * Register range: 0xD0-0xD1, Slot: 0x02
 */
static bool handle_valve_label(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    uint8_t zone_num = reg_id - 0xD0 + 1;

    // Safely copy string from payload — protocol does not guarantee null termination
    char label[32] = {0};
    int str_len = payload_len - 2;
    if (str_len > (int)sizeof(label) - 1) str_len = (int)sizeof(label) - 1;
    memcpy(label, &payload[2], str_len);

    ESP_LOGI(TAG, "%s Valve zone %d label (0x%02X) - \"%s\"", addr_info, zone_num, reg_id, label);

    // Update pool state
    pool_state_t state_snapshot;
    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for valve label");
        return true;
    }
    int slot = -1;
    for (int i = 0; i < MAX_REGISTER_LABELS; i++) {
        if (ctx->pool_state->register_labels[i].valid && ctx->pool_state->register_labels[i].reg_id == reg_id) {
            slot = i;
            break;
        } else if (!ctx->pool_state->register_labels[i].valid && slot == -1) {
            slot = i;
        }
    }

    if (slot >= 0) {
        ctx->pool_state->register_labels[slot].reg_id = reg_id;
        strncpy(ctx->pool_state->register_labels[slot].label, label, sizeof(ctx->pool_state->register_labels[slot].label) - 1);
        ctx->pool_state->register_labels[slot].label[sizeof(ctx->pool_state->register_labels[slot].label) - 1] = '\0';
        ctx->pool_state->register_labels[slot].valid = true;
        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    }

    // Also store directly in valve state for MQTT name-change detection
    int valve_idx = reg_id - 0xD0;
    if (valve_idx >= 0 && valve_idx < MAX_VALVE_SLOTS) {
        strncpy(ctx->pool_state->valves[valve_idx].name, label,
                sizeof(ctx->pool_state->valves[valve_idx].name) - 1);
        ctx->pool_state->valves[valve_idx].name[sizeof(ctx->pool_state->valves[valve_idx].name) - 1] = '\0';
    }

    state_snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    // Re-publish valve discovery and state now that the name is known
    if (ctx->enable_mqtt && zone_num >= 1 && zone_num <= MAX_VALVE_SLOTS) {
        mqtt_publish_valve(&state_snapshot, zone_num);
    }

    return true;
}

/**
 * Handler: Favourite/mode label
 * Register range: 0x31–0x38, Slot: 0x03
 * Index 0=Pool, 1=Spa, 2–7=Favourites 1–6
 */
static bool handle_favourite_label(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    int index = reg_id - 0x31;
    if (index < 0 || index >= MAX_FAVOURITES) return false;

    char label[32] = {0};
    int str_len = payload_len - 2;
    if (str_len > (int)sizeof(label) - 1) str_len = (int)sizeof(label) - 1;
    memcpy(label, &payload[2], str_len);

    ESP_LOGI(TAG, "%s Favourite %d label (0x%02X) - \"%s\"", addr_info, index, reg_id, label);

    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for favourite label");
        return true;
    }
    strncpy(ctx->pool_state->favourites[index].name, label,
            sizeof(ctx->pool_state->favourites[index].name) - 1);
    ctx->pool_state->favourites[index].name[sizeof(ctx->pool_state->favourites[index].name) - 1] = '\0';
    ctx->pool_state->favourites[index].name_valid = true;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    pool_state_t state_snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    mqtt_publish_favourite(&state_snapshot);
    return true;
}

/**
 * Handler: Favourite/mode enable flag

 * Register range: 0x21–0x28, Slot: 0x03
 * Index 0=Pool, 1=Spa, 2–7=Favourites 1–6
 */
static bool handle_favourite_enable(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 3) return false;

    uint8_t reg_id = payload[0];
    int index = reg_id - 0x21;
    if (index < 0 || index >= MAX_FAVOURITES) return false;

    bool enabled = (payload[2] != 0x00);

    ESP_LOGI(TAG, "%s Favourite %d (0x%02X) - %s", addr_info, index, reg_id,
             enabled ? "enabled" : "disabled");

    if (!ctx->state_mutex || xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for favourite enable");
        return true;
    }
    ctx->pool_state->favourites[index].enabled = enabled;
    ctx->pool_state->favourites[index].enabled_valid = true;
    ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    pool_state_t state_snapshot = *ctx->pool_state;
    xSemaphoreGive(ctx->state_mutex);

    mqtt_publish_favourite(&state_snapshot);
    return true;
}


/**
 * Handler: Active channels bitmask message
 * Pattern: "02 00 50 00 6F 80 00 0D 0D 5B"
 */
static bool handle_channels(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t bitmask = payload[0];
    ESP_LOGI(TAG, "%s Active channels - 0x%02X [%c%c%c%c%c%c%c%c]",
             addr_info, bitmask,
             (bitmask & 0x80) ? '8' : '-',
             (bitmask & 0x40) ? '7' : '-',
             (bitmask & 0x20) ? '6' : '-',
             (bitmask & 0x10) ? '5' : '-',
             (bitmask & 0x08) ? '4' : '-',
             (bitmask & 0x04) ? '3' : '-',
             (bitmask & 0x02) ? '2' : '-',
             (bitmask & 0x01) ? '1' : '-');

    return true;
}

/**
 * Handler: Channel status message (most complex handler)
 * Pattern: "02 00 50 FF FF 80 00 0B 25 00"
 */
static bool handle_channel_status(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    if (payload_len < 1) return false;

    uint8_t num_channels = payload[0];
    if (num_channels > MAX_CHANNELS) {
        ESP_LOGW(TAG, "%s Channel count %d exceeds MAX_CHANNELS (%d), clamping",
                 addr_info, num_channels, MAX_CHANNELS);
        num_channels = MAX_CHANNELS;
    }
    ESP_LOGI(TAG, "%s Channel status (%d channels):", addr_info, num_channels);

    int payload_idx = 1;  // Channel data starts at payload[1]
    int ch_num = 1;
    bool past_end = false;
    uint8_t channels_to_publish[MAX_CHANNELS] = {0};
    int num_to_publish = 0;

    // Update pool state
    pool_state_t state_snapshot;
    if (ctx->state_mutex && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        ctx->pool_state->num_channels = num_channels;

        while (ch_num <= num_channels) {
            if (past_end || payload_idx + 2 >= payload_len) {
                ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                ch_num++;
                continue;
            }

            uint8_t ch_type = payload[payload_idx];
            uint8_t state   = payload[payload_idx + 1];
            uint8_t active  = payload[payload_idx + 2];
            const char *state_name = (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";

            if (ch_type == CHANNEL_UNUSED) {
                ESP_LOGI(TAG, "  Ch%d: Unused", ch_num);
                ctx->pool_state->channels[ch_num - 1].configured = false;
            } else {
                const char *type_name = get_channel_type_name(ch_type);
                ESP_LOGI(TAG, "  Ch%d: %s (%d) = %s (%s)", ch_num, type_name, ch_type, state_name,
                         active ? "Active" : "Inactive");

                // Update channel state
                ctx->pool_state->channels[ch_num - 1].id = ch_num;
                ctx->pool_state->channels[ch_num - 1].type = ch_type;
                ctx->pool_state->channels[ch_num - 1].state = state;
                ctx->pool_state->channels[ch_num - 1].active = (active != 0);
                ctx->pool_state->channels[ch_num - 1].configured = true;

                // Mark this channel for publishing
                channels_to_publish[num_to_publish++] = ch_num;
            }

            payload_idx += 3;
            ch_num++;
        }

        ctx->pool_state->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        state_snapshot = *ctx->pool_state;
        xSemaphoreGive(ctx->state_mutex);
    }

    // Publish all channels using snapshot (outside mutex)
    if (ctx->enable_mqtt) {
        for (int i = 0; i < num_to_publish; i++) {
            mqtt_publish_channel(&state_snapshot, channels_to_publish[i]);
        }
    }

    return true;
}

// ======================================================
// Main decoder function
// ======================================================

static bool dispatch_message(const uint8_t *data, int len, const uint8_t *payload, int payload_len, const char *addr_info, message_decoder_context_t *ctx);

bool decode_message(const uint8_t *data, int len, message_decoder_context_t *ctx)
{
    if (!ctx || !ctx->pool_state) {
        return false;
    }

    // Minimum valid message: 10-byte header + data checksum + end byte
    if (len < 12 || data[0] != 0x02 || data[len - 1] != 0x03) {
        return false;
    }

    // Log full message before decoding
    int full_msg_size = 3 * len + 1;
    char *full_msg = malloc(full_msg_size);
    if (!full_msg) return false;
    int msg_pos = 0;
    for (int i = 0; i < len && msg_pos < full_msg_size - 3; i++) {
        msg_pos += snprintf(&full_msg[msg_pos], full_msg_size - msg_pos, "%02X ", data[i]);
    }
    full_msg[msg_pos] = '\0';
    ESP_LOGI(TAG, "RX MSG: %s", full_msg);
    free(full_msg);

    // Validate length field: byte[8] = total message length including START and END
    if (data[8] != len) {
        ESP_LOGW(TAG, "Length field mismatch: byte[8]=0x%02X (%d), actual=%d",
                 data[8], data[8], len);
    }

    // Validate header checksum: byte[9] = sum(bytes 0-8) & 0xFF
    uint8_t expected_hchk = 0;
    for (int i = 0; i < 9; i++) expected_hchk += data[i];
    if (expected_hchk != data[9]) {
        ESP_LOGW(TAG, "Header checksum FAILED: expected 0x%02X, got 0x%02X",
                 expected_hchk, data[9]);
    }

    // Validate data checksum
    if (!verify_message_checksum(data, len)) {
        uint32_t sum = 0;
        for (int i = 10; i < len - 2; i++) sum += data[i];
        ESP_LOGW(TAG, "Data checksum FAILED: expected 0x%02X, got 0x%02X",
                 (uint8_t)(sum & 0xFF), data[len - 2]);
    }

    // Extract data payload section (bytes 10 to len-3)
    // Message format: [START=0][SRC=1-2][DST=3-4][CTRL=5-6][CMD=7][LEN=8][HDR_CHK=9][DATA=10...][DATA_CHK=len-2][END=len-1]
    const uint8_t *payload = &data[10];
    int payload_len = len - 12;  // len - (10 header bytes + data checksum + end)

    // Extract source and destination addresses
    uint8_t src_hi = data[1], src_lo = data[2];
    uint8_t dst_hi = data[3], dst_lo = data[4];
    char src_name_buf[16], dst_name_buf[16];
    const char *src_name = get_device_name(src_hi, src_lo, src_name_buf, sizeof(src_name_buf));
    const char *dst_name = get_device_name(dst_hi, dst_lo, dst_name_buf, sizeof(dst_name_buf));

    char addr_info[64];
    snprintf(addr_info, sizeof(addr_info), "[%s -> %s]", src_name, dst_name);

    // Register source address in the seen-devices registry (skip broadcast)
    if (ctx->state_mutex && !(src_hi == 0xFF && src_lo == 0xFF)) {
        if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            find_or_insert_seen_device_locked(ctx->pool_state, src_hi, src_lo);
            xSemaphoreGive(ctx->state_mutex);
        }
    }

    bool decoded = dispatch_message(data, len, payload, payload_len, addr_info, ctx);

    // Increment global and per-device decoded/unknown counters
    if (ctx->state_mutex) {
        if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            if (decoded) ctx->pool_state->messages_decoded_total++;
            else         ctx->pool_state->messages_unknown_total++;

            // Per-device counters (skip broadcast)
            if (!(src_hi == 0xFF && src_lo == 0xFF)) {
                int idx = find_or_insert_seen_device_locked(ctx->pool_state, src_hi, src_lo);
                if (idx >= 0) {
                    if (decoded) ctx->pool_state->seen_devices[idx].decoded_count++;
                    else         ctx->pool_state->seen_devices[idx].unknown_count++;
                }
            }
            xSemaphoreGive(ctx->state_mutex);
        }
    }

    return decoded;
}

static bool dispatch_message(
    const uint8_t *data, int len,
    const uint8_t *payload, int payload_len,
    const char *addr_info,
    message_decoder_context_t *ctx)
{
    // Firmware version (CMD 0x0A) — universal across sources; payload layout
    // is identical regardless of which device is broadcasting, so dispatched
    // by command byte rather than per-source pattern. See PROTOCOL.md
    // command `0x0A` section and the Known Command Bytes "Firmware version" row.
    if (data[7] == 0x0A) {
        return handle_firmware_version(data, len, payload, payload_len, addr_info, ctx);
    }

    // Water temperature reading — CMD 0x16 (canonical) and CMD 0x31 (alt/
    // log-only) share the same {temp1, temp2} layout and are routed through
    // the same handler. See PROTOCOL.md commands `0x16` and `0x31`.
    if (data[7] == 0x16 || data[7] == 0x31) {
        return handle_temp_reading(data, len, payload, payload_len, addr_info, ctx);
    }

    // Temperature setpoint command (CMD 0x19) — source-agnostic. Routed by
    // the slot byte (payload[0]): 0x01/0x02 = Pool/Spa setpoint (3-byte
    // payload, Gateway-sourced); 0x03 = heater pair setpoint (5-byte payload
    // in °C + °F, Touchscreen-sourced to 0x007F).
    if (data[7] == 0x19 && payload_len >= 1) {
        if (payload[0] == 0x01 || payload[0] == 0x02) {
            return handle_temp_set_cmd_pool_spa(data, len, payload, payload_len, addr_info, ctx);
        }
        if (payload[0] == 0x03) {
            return handle_temp_set_cmd_heaters(data, len, payload, payload_len, addr_info, ctx);
        }
    }

    // Chlorinator setpoint (CMD 0x1D) and reading (CMD 0x1F) — same
    // `{channel, value_lo, value_hi}` payload from both 0x0090 RolaChem and
    // 0x0084 Viron, only the source address (and resulting checksum1 byte)
    // differs. Dispatched by CMD byte and routed by the channel byte
    // (payload[0]): 0x01=pH, 0x02=ORP.
    if (data[7] == 0x1D && payload_len >= 1) {
        if (payload[0] == 0x01) {
            return handle_chlor_ph_setpoint(data, len, payload, payload_len, addr_info, ctx);
        }
        if (payload[0] == 0x02) {
            return handle_chlor_orp_setpoint(data, len, payload, payload_len, addr_info, ctx);
        }
    }
    if (data[7] == 0x1F && payload_len >= 1) {
        if (payload[0] == 0x01) {
            return handle_chlor_ph_reading(data, len, payload, payload_len, addr_info, ctx);
        }
        if (payload[0] == 0x02) {
            return handle_chlor_orp_reading(data, len, payload, payload_len, addr_info, ctx);
        }
    }

    // Register read request (CMD 0x39) — source-agnostic. Observed from the
    // Internet Gateway (0x00F0) and the Genus Heater (0x0070); same
    // `{reg_id, slot_id}` payload from either source.
    if (data[7] == 0x39) {
        return handle_register_read_request(data, len, payload, payload_len, addr_info, ctx);
    }

    // Register data (CMD 0x38) — source-agnostic. Routed by (reg_id, slot)
    // via REGISTER_HANDLERS. The Touchscreen (0x0050) is the canonical
    // responder to gateway 0x39 reads, but the same payload shape is used
    // wherever 0x38 originates.
    if (data[7] == 0x38) {
        // Extract register ID and slot
        if (payload_len < 2) {
            ESP_LOGW(TAG, "%s Register message - Payload too short", addr_info);
            return false;
        }

        uint8_t reg_id = payload[0];
        uint8_t slot = payload[1];

        // ESP_LOGI(TAG, "%s Register message received - Reg=0x%02X, Slot=0x%02X, searching handlers...",
        //          addr_info, reg_id, slot);

        // Find matching handler in dispatch table
        for (int i = 0; i < REGISTER_HANDLER_COUNT; i++) {
            const register_handler_t *entry = &REGISTER_HANDLERS[i];

            if (reg_id >= entry->reg_start && reg_id <= entry->reg_end && entry->slot == slot) {
                // ESP_LOGI(TAG, "  -> Matched handler: %s", entry->name);
                return entry->handler(data, len, payload, payload_len, addr_info, ctx);
            }
        }

        // If we are here it means we have an unhandled register message - log details for debugging
        
        // Format all payload bytes as hex for debugging (e.g., "7F 02 00 81")
        int payload_hex_size = payload_len * 3 + 1;
        char *payload_hex = malloc(payload_hex_size);
        if (!payload_hex) return false;
        int pos = 0;
        for (int i = 0; i < payload_len; i++) {
            pos += snprintf(&payload_hex[pos], payload_hex_size - pos, "%02X ", payload[i]);
        }
        // Remove trailing space
        if (pos > 0 && payload_hex[pos - 1] == ' ') {
            payload_hex[pos - 1] = '\0';
        }

        ESP_LOGW(TAG, "%s Unhandled register - Reg=0x%02X, Slot=0x%02X, Payload[%d]: %s",
                 addr_info, reg_id, slot, payload_len, payload_hex);
        free(payload_hex);
        return false;
    }

    // Configuration messages
    if (match_pattern(data, len, MSG_TYPE_LIGHT_CONFIG)) {
        return handle_light_config(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_CONFIG)) {
        return handle_config(data, len, payload, payload_len, addr_info, ctx);
    }

    // Operational messages
    if (match_pattern(data, len, MSG_TYPE_MODE)) {
        return handle_mode(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_CHANNELS)) {
        return handle_channels(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_CHANNEL_STATUS)) {
        return handle_channel_status(data, len, payload, payload_len, addr_info, ctx);
    }

    // Temperature messages
    if (match_pattern(data, len, MSG_TYPE_TEMP_SETTING)) {
        return handle_temp_setting(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_HEATER)) {
        return handle_heater(data, len, payload, payload_len, addr_info, ctx);
    }

    // Genus Heater (0x0070) messages
    if (match_pattern(data, len, MSG_TYPE_GENUS_HEATER_TEMP_SETTING)) {
        return handle_genus_heater_temp_setting(data, len, payload, payload_len, addr_info, ctx);
    }

    // ICI Gas Heater (0x0074) messages
    if (match_pattern(data, len, MSG_TYPE_ICI_HEATER_STATUS)) {
        return handle_ici_heater_status(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_ICI_HEATER_TEMP_SETTING)) {
        return handle_ici_heater_temp_setting(data, len, payload, payload_len, addr_info, ctx);
    }

    // Chlorinator status broadcast (§32) — both 0x0090 and 0x0084 variants
    if (match_pattern(data, len, MSG_TYPE_CHLOR_STATUS_A) ||
        match_pattern(data, len, MSG_TYPE_CHLOR_STATUS_B)) {
        return handle_chlor_status(data, len, payload, payload_len, addr_info, ctx);
    }

    // Gateway messages
    if (match_pattern(data, len, MSG_TYPE_SERIAL_NUMBER)) {
        return handle_serial_number(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_GATEWAY_IP)) {
        return handle_gateway_ip(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_GATEWAY_COMMS)) {
        return handle_gateway_comms(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_GATEWAY_STATUS)) {
        return handle_gateway_status(data, len, payload, payload_len, addr_info, ctx);
    }

    // Gateway control commands
    if (match_pattern(data, len, MSG_TYPE_CHANNEL_TOGGLE_CMD)) {
        return handle_channel_toggle_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_LIGHT_CONTROL_CMD)) {
        return handle_light_control_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_MODE_CONTROL_CMD)) {
        return handle_mode_control_cmd(data, len, payload, payload_len, addr_info, ctx);
    }

    // Controller info messages
    if (match_pattern(data, len, MSG_TYPE_CONTROLLER_TIME)) {
        return handle_controller_time(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_TOUCHSCREEN_UNKNOWN1)) {
        return handle_touchscreen_unknown1(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_VALVE_STATE)) {
        return handle_valve_state(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_TOUCHSCREEN_UNKNOWN2)) {
        return handle_touchscreen_unknown2(data, len, payload, payload_len, addr_info, ctx);
    }

    if (match_pattern(data, len, MSG_TYPE_TOUCHSCREEN_UNKNOWN3)) {
        return handle_touchscreen_unknown3(data, len, payload, payload_len, addr_info, ctx);
    }

    // No handler matched - log as unknown
    return handle_unknown(data, len, payload, payload_len, addr_info, ctx);
}
