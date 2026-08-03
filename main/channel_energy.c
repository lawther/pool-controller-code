#include "channel_energy.h"
#include "config.h"
#include "pool_state.h"
#include "channel_power.h"
#include "mqtt_publish.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "CHANNEL_ENERGY";

// Sampling interval: short enough that a channel toggling on/off mid-interval
// only introduces a small integration error.
#define CHANNEL_ENERGY_SAMPLE_INTERVAL_MS   (10 * 1000)

// Publish interval, decoupled from sampling. A cumulative meter only needs to
// be published often enough for Home Assistant to bucket it; publishing every
// sample floods the HA event log and recorder database with rows that carry no
// extra information. 5 minutes matches HA's long-term statistics resolution.
#define CHANNEL_ENERGY_PUBLISH_INTERVAL_MS  (5 * 60 * 1000)

static double s_energy_kwh[MAX_CHANNELS];
static double s_system_energy_kwh;

// Per-channel: energy has been accumulated that hasn't been published yet.
static bool s_energy_pending[MAX_CHANNELS];

static void channel_energy_task(void *arg)
{
    (void)arg;
    TickType_t last_tick = xTaskGetTickCount();
    // Back-date so the first sample publishes: the accumulator lives in RAM and
    // restarts at 0 on boot, and the broker is still holding the retained
    // pre-reboot total, so the reset needs to reach HA promptly rather than a
    // publish interval later.
    TickType_t last_publish_tick = last_tick - pdMS_TO_TICKS(CHANNEL_ENERGY_PUBLISH_INTERVAL_MS);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHANNEL_ENERGY_SAMPLE_INTERVAL_MS));

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

        bool publish_due = (TickType_t)(now_tick - last_publish_tick) >=
                           pdMS_TO_TICKS(CHANNEL_ENERGY_PUBLISH_INTERVAL_MS);
        if (publish_due) {
            last_publish_tick = now_tick;
        }

        for (uint8_t ch = 1; ch <= MAX_CHANNELS; ch++) {
            const channel_state_t *channel = &snapshot.channels[ch - 1];

            uint16_t watts = 0;
            if (!channel->configured ||
                !channel_power_get_effective(&snapshot, ch, &watts)) {
                watts = 0;  // Unconfigured, or no power source (telemetry or manual estimate)
            }

            if (watts > 0) {
                s_energy_kwh[ch - 1] += (watts / 1000.0) * interval_hours;
                // Debug level: this fires per active channel every interval, which
                // would drown the log at INFO, but it's the only view of what the
                // accumulator is crediting when a total looks wrong.
                ESP_LOGD(TAG, "Channel %d: %u W -> %.4f kWh cumulative",
                         (int)ch, (unsigned)watts, s_energy_kwh[ch - 1]);
                s_energy_pending[ch - 1] = true;
            } else if (!s_energy_pending[ch - 1]) {
                // Not drawing and nothing accumulated since the last publish:
                // the total can't have moved, so republishing it would just
                // resend the value already retained on the broker, forever.
                continue;
            }

            // Publish on the interval, and once more on the sample where a
            // channel stops drawing, so a run's final total lands in the right
            // HA time bucket instead of up to a publish interval later.
            if (publish_due || watts == 0) {
                mqtt_publish_channel_energy(ch, s_energy_kwh[ch - 1]);
                s_energy_pending[ch - 1] = false;
            }
        }

        // System baseline: the idle draw that belongs to no channel. It has no
        // active state to gate on — if a baseline is configured, it is drawing,
        // so this accumulates every interval the device is up.
        uint16_t system_watts = channel_power_get_system();
        if (system_watts > 0) {
            s_system_energy_kwh += (system_watts / 1000.0) * interval_hours;
            if (publish_due) {
                mqtt_publish_system_energy(s_system_energy_kwh);
            }
        }
    }
}

void channel_energy_start(void)
{
    ESP_LOGI(TAG, "Starting channel energy accumulator (sample %.0fs, publish %.0fs)",
             CHANNEL_ENERGY_SAMPLE_INTERVAL_MS / 1000.0,
             CHANNEL_ENERGY_PUBLISH_INTERVAL_MS / 1000.0);
    // 8192 to match tcp_bridge's decoder task: both copy the full ~3KB
    // pool_state_t onto the stack (see the snapshot above)
    xTaskCreate(channel_energy_task, "chan_energy", 8192, NULL, 1, NULL);
}
