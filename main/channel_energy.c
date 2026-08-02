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
static double s_system_energy_kwh;

static void channel_energy_task(void *arg)
{
    (void)arg;
    TickType_t last_tick = xTaskGetTickCount();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHANNEL_ENERGY_INTERVAL_MS));

        if (!s_pool_state_mutex || xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
            continue;  // last_tick untouched — the next pass covers this interval too
        }
        pool_state_t snapshot = s_pool_state;
        xSemaphoreGive(s_pool_state_mutex);

        // Integrate over the time that actually elapsed rather than the nominal
        // interval: the loop body's own work adds to the delay, and this task
        // runs at the lowest priority so it can be preempted for arbitrarily
        // long. Crediting a fixed interval would always undercount, and the
        // error compounds without bound in a cumulative meter. Unsigned
        // subtraction gives the correct delta across tick-count wraparound.
        TickType_t now_tick = xTaskGetTickCount();
        double interval_hours = ((double)(TickType_t)(now_tick - last_tick) * portTICK_PERIOD_MS) / 3600000.0;
        last_tick = now_tick;

        for (uint8_t ch = 1; ch <= MAX_CHANNELS; ch++) {
            const channel_state_t *channel = &snapshot.channels[ch - 1];

            if (!channel->configured) {
                continue;
            }

            uint16_t watts;
            if (!channel_power_get_effective(&snapshot, ch, &watts)) {
                continue;  // No power source (telemetry or manual estimate) configured
            }

            if (watts == 0) {
                // Channel is off (or drawing nothing): the total can't have moved,
                // so republishing it would just resend the value already retained
                // on the broker every interval, forever.
                continue;
            }

            s_energy_kwh[ch - 1] += (watts / 1000.0) * interval_hours;
            // ESP_LOGI(TAG, "Channel %d: %u W -> %.4f kWh cumulative", ch, watts, s_energy_kwh[ch - 1]);
            mqtt_publish_channel_energy(ch, s_energy_kwh[ch - 1]);
        }

        // System baseline: the idle draw that belongs to no channel. It has no
        // active state to gate on — if a baseline is configured, it is drawing,
        // so this accumulates every interval the device is up.
        uint16_t system_watts = channel_power_get_system();
        if (system_watts > 0) {
            s_system_energy_kwh += (system_watts / 1000.0) * interval_hours;
            mqtt_publish_system_energy(s_system_energy_kwh);
        }
    }
}

void channel_energy_start(void)
{
    // ESP_LOGI(TAG, "Starting channel energy accumulator (%.0fs interval)",
    //          CHANNEL_ENERGY_INTERVAL_MS / 1000.0);
    // 8192 to match tcp_bridge's decoder task: both copy the full ~3KB
    // pool_state_t onto the stack (see the snapshot above)
    xTaskCreate(channel_energy_task, "chan_energy", 8192, NULL, 1, NULL);
}
