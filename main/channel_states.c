/**
 * Channel state name table
 *
 * The state codes broadcast per channel by CMD 0x0B, and accepted as the
 * target state by CMD 0x0F — see PROTOCOL.md, 0x0B Channel Status. The first
 * three (Off/Auto/On) are also the whole code space of the protocol's other
 * 0/1/2 state fields: light zone state, valve state and solar mode.
 *
 * These strings are also the MQTT select options (pump mode, valves),
 * published by mqtt_discovery.c and parsed back by mqtt_commands.c, so a
 * second copy of them would be a silent drift risk.
 *
 * Kept in its own translation unit, so the host-side unit tests can link the
 * real table without pulling in all of message_decoder.c.
 */

#include <string.h>
#include <strings.h>
#include "pool_state.h"

const char *CHANNEL_STATE_NAMES[] = {
    "Off",      // 0
    "Auto",     // 1
    "On",       // 2
    "Low",      // 3
    "Medium",   // 4
    "High",     // 5
};

const char *channel_state_name(uint8_t state)
{
    return (state < CHANNEL_STATE_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";
}

const char *off_auto_on_name(uint8_t state)
{
    return (state < OFF_AUTO_ON_COUNT) ? CHANNEL_STATE_NAMES[state] : "Unknown";
}

int channel_state_parse(const char *payload, int payload_len, int count)
{
    for (int i = 0; i < count && i < CHANNEL_STATE_COUNT; i++) {
        if ((int)strlen(CHANNEL_STATE_NAMES[i]) == payload_len &&
            strncasecmp(payload, CHANNEL_STATE_NAMES[i], payload_len) == 0) {
            return i;
        }
    }
    return -1;
}
