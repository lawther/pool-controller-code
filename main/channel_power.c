#include "channel_power.h"
#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "CHANNEL_POWER";

#define CHANNEL_POWER_NVS_NAMESPACE "channel_pwr"

// NVS key for the system baseline. Per-channel keys are "w0".."w<N-1>", so a
// non-numeric suffix can never collide with one.
#define SYSTEM_POWER_NVS_KEY "wsys"

static uint16_t s_configured_watts[MAX_CHANNELS];
static uint16_t s_system_watts;

void channel_power_init(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CHANNEL_POWER_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No channel power config in NVS yet");
        return;
    }

    for (int i = 0; i < MAX_CHANNELS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "w%d", i);
        uint16_t watts = 0;
        if (nvs_get_u16(nvs_handle, key, &watts) == ESP_OK) {
            s_configured_watts[i] = watts;
        }
    }

    uint16_t system_watts = 0;
    if (nvs_get_u16(nvs_handle, SYSTEM_POWER_NVS_KEY, &system_watts) == ESP_OK) {
        s_system_watts = system_watts;
    }
    nvs_close(nvs_handle);
}

// Write one wattage to NVS. Shared by the per-channel and system setters,
// which differ only in the key and the cache they update.
static esp_err_t save_watts(const char *key, uint16_t watts)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CHANNEL_POWER_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u16(nvs_handle, key, watts);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

uint16_t channel_power_get_configured(uint8_t channel_id)
{
    if (channel_id < 1 || channel_id > MAX_CHANNELS) {
        return 0;
    }
    return s_configured_watts[channel_id - 1];
}

esp_err_t channel_power_set_configured(uint8_t channel_id, uint16_t watts)
{
    if (channel_id < 1 || channel_id > MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[8];
    snprintf(key, sizeof(key), "w%d", channel_id - 1);
    esp_err_t err = save_watts(key, watts);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save channel %d power: %s", channel_id, esp_err_to_name(err));
        return err;
    }

    s_configured_watts[channel_id - 1] = watts;
    ESP_LOGI(TAG, "Channel %d configured power set to %u W", channel_id, watts);
    return ESP_OK;
}

uint16_t channel_power_get_system(void)
{
    return s_system_watts;
}

esp_err_t channel_power_set_system(uint16_t watts)
{
    esp_err_t err = save_watts(SYSTEM_POWER_NVS_KEY, watts);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save system power: %s", esp_err_to_name(err));
        return err;
    }

    s_system_watts = watts;
    ESP_LOGI(TAG, "System baseline power set to %u W", watts);
    return ESP_OK;
}

bool channel_power_has_telemetry(const pool_state_t *state, uint8_t channel_id)
{
    if (!state || channel_id < 1 || channel_id > MAX_CHANNELS) {
        return false;
    }

    const channel_state_t *channel = &state->channels[channel_id - 1];

    // Real device telemetry takes precedence over a manual estimate — but only
    // once an actual power figure has been received. The pump also broadcasts
    // speed-only telemetry (CMD 0x3B with a 2-byte payload), which sets
    // pump_telemetry_seen without ever populating pump_power_watts; taking this
    // branch on that alone would report a permanent 0 W and lock out the manual
    // estimate with no way for the user to correct it.
    return channel->type == CHANNEL_TYPE_FILTER &&
           state->pump_telemetry_seen && state->pump_power_watts_valid;
}

bool channel_power_get_effective(const pool_state_t *state, uint8_t channel_id, uint16_t *out_watts)
{
    if (channel_id < 1 || channel_id > MAX_CHANNELS || !out_watts) {
        return false;
    }

    const channel_state_t *channel = &state->channels[channel_id - 1];

    if (channel_power_has_telemetry(state, channel_id)) {
        *out_watts = state->pump_power_watts;
        return true;
    }

    uint16_t configured = channel_power_get_configured(channel_id);
    if (configured == 0) {
        return false;  // Unconfigured — nothing to report
    }

    *out_watts = channel->active ? configured : 0;
    return true;
}
