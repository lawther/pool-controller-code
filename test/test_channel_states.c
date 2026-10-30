/**
 * Unit tests for the channel state name table and its helpers
 * (main/channel_states.c).
 *
 * To run:
 *   cd test && gcc -I. -I.. -o run_channel_states test_channel_states.c ../main/channel_states.c && ./run_channel_states
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../main/pool_state.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            printf("  ✓ PASS: %s\n", message); \
            tests_passed++; \
        } else { \
            printf("  ✗ FAIL: %s\n", message); \
            tests_failed++; \
        } \
    } while(0)

// Parse a NUL-terminated payload, as an MQTT handler would with its length
static int parse(const char *payload, int count)
{
    return channel_state_parse(payload, (int)strlen(payload), count);
}

// The selects publish CHANNEL_STATE_NAMES as their options and read the state
// topic back against them, so any state the decoder can record for a channel
// has to have a name in that table.
void test_channel_state_names_cover_every_state(void)
{
    for (int i = 0; i < CHANNEL_STATE_COUNT; i++) {
        TEST_ASSERT(CHANNEL_STATE_NAMES[i] != NULL && CHANNEL_STATE_NAMES[i][0] != '\0',
                    "Every channel state code has a name");
    }
}

void test_channel_state_name(void)
{
    TEST_ASSERT(strcmp(channel_state_name(0), "Off") == 0, "0 is Off");
    TEST_ASSERT(strcmp(channel_state_name(2), "On") == 0, "2 is On");
    TEST_ASSERT(strcmp(channel_state_name(5), "High") == 0, "5 is High");
    TEST_ASSERT(strcmp(channel_state_name(6), "Unknown") == 0, "6 is Unknown");
}

// Off/Auto/On fields must never name a pump speed: a stray 3 published as
// "Low" would not be one of a valve select's options.
void test_off_auto_on_name(void)
{
    TEST_ASSERT(strcmp(off_auto_on_name(0), "Off") == 0, "0 is Off");
    TEST_ASSERT(strcmp(off_auto_on_name(1), "Auto") == 0, "1 is Auto");
    TEST_ASSERT(strcmp(off_auto_on_name(2), "On") == 0, "2 is On");
    TEST_ASSERT(strcmp(off_auto_on_name(3), "Unknown") == 0, "3 is Unknown, not Low");
}

// Every name parses back to its own code, so the select options round-trip
void test_parse_round_trips_every_name(void)
{
    for (int i = 0; i < CHANNEL_STATE_COUNT; i++) {
        TEST_ASSERT(parse(CHANNEL_STATE_NAMES[i], CHANNEL_STATE_COUNT) == i,
                    "Name parses back to its state code");
    }
}

// HA's light entity sends ON/OFF in upper case
void test_parse_ignores_case(void)
{
    TEST_ASSERT(parse("ON", OFF_AUTO_ON_COUNT) == 2, "ON is 2");
    TEST_ASSERT(parse("OFF", OFF_AUTO_ON_COUNT) == 0, "OFF is 0");
    TEST_ASSERT(parse("AUTO", OFF_AUTO_ON_COUNT) == 1, "AUTO is 1");
    TEST_ASSERT(parse("medium", CHANNEL_STATE_COUNT) == 4, "medium is 4");
}

// A prefix or extension of a name is not that name
void test_parse_requires_exact_length(void)
{
    TEST_ASSERT(parse("O", OFF_AUTO_ON_COUNT) == -1, "O is rejected");
    TEST_ASSERT(parse("", OFF_AUTO_ON_COUNT) == -1, "Empty payload is rejected");
    TEST_ASSERT(parse("Onn", OFF_AUTO_ON_COUNT) == -1, "Onn is rejected");
    TEST_ASSERT(parse("Au", OFF_AUTO_ON_COUNT) == -1, "Au is rejected");

    // Length-bounded: a longer buffer whose first payload_len bytes are a name
    TEST_ASSERT(channel_state_parse("OnXYZ", 2, OFF_AUTO_ON_COUNT) == 2,
                "Only payload_len bytes are compared");
}

void test_parse_honours_count(void)
{
    TEST_ASSERT(parse("Low", OFF_AUTO_ON_COUNT) == -1, "Low is not an Off/Auto/On state");
    TEST_ASSERT(parse("Low", CHANNEL_STATE_COUNT) == 3, "Low is a channel state");
    TEST_ASSERT(parse("Blink", CHANNEL_STATE_COUNT) == -1, "Unknown name is rejected");
}

int main(void)
{
    printf("\n======================================\n");
    printf("  Channel States Unit Tests\n");
    printf("======================================\n");

    printf("\n--- Name Tests ---\n");
    test_channel_state_names_cover_every_state();
    test_channel_state_name();
    test_off_auto_on_name();

    printf("\n--- Parse Tests ---\n");
    test_parse_round_trips_every_name();
    test_parse_ignores_case();
    test_parse_requires_exact_length();
    test_parse_honours_count();

    printf("\n======================================\n");
    printf("  Test Summary\n");
    printf("======================================\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("======================================\n\n");

    return (tests_failed == 0) ? 0 : 1;
}
