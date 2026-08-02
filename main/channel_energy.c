#include "channel_energy.h"
#include "config.h"
#include "pool_state.h"
#include "channel_power.h"
#include "mqtt_publish.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "CHANNEL_ENERGY";

// Sample/publish interval. Short enough that a channel toggling on/off
// mid-interval only introduces a small integration error, long enough that
// the MQTT traffic (one small message per active channel) is negligible.
#define CHANNEL_ENERGY_INTERVAL_MS  (10 * 1000)

static double s_energy_kwh[MAX_CHANNELS];

static void channel_energy_task(void *arg)
{
    (void)arg;
    const double interval_hours = (CHANNEL_ENERGY_INTERVAL_MS / 1000.0) / 3600.0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHANNEL_ENERGY_INTERVAL_MS));

        if (!s_pool_state_mutex || xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
            continue;
        }
        pool_state_t snapshot = s_pool_state;
        xSemaphoreGive(s_pool_state_mutex);

        for (uint8_t ch = 1; ch <= MAX_CHANNELS; ch++) {
            const channel_state_t *channel = &snapshot.channels[ch - 1];

            if (!channel->configured) {
                continue;
            }

            uint16_t watts;
            if (!channel_power_get_effective(&snapshot, ch, &watts)) {
                continue;  // No power source (telemetry or manual estimate) configured
            }

            s_energy_kwh[ch - 1] += (watts / 1000.0) * interval_hours;
            ESP_LOGI(TAG, "Channel %d: %u W -> %.4f kWh cumulative", ch, watts, s_energy_kwh[ch - 1]);
            mqtt_publish_channel_energy(ch, s_energy_kwh[ch - 1]);
        }
    }
}

void channel_energy_start(void)
{
    ESP_LOGI(TAG, "Starting channel energy accumulator (%.0fs interval)",
             CHANNEL_ENERGY_INTERVAL_MS / 1000.0);
    // 8192 to match tcp_bridge's decoder task: both copy the full ~3KB
    // pool_state_t onto the stack (see the snapshot above), which a smaller
    // stack (the original 3072 here) isn't enough to safely hold alongside
    // normal call overhead.
    xTaskCreate(channel_energy_task, "chan_energy", 8192, NULL, 1, NULL);
}
