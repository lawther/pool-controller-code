#include "firmware_update.h"
#include "config.h"
#include "mqtt_poolclient.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"

#include "cJSON.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "FW_UPDATE";

// Cached status, protected by s_status_lock.
static fw_update_status_t s_status;
static SemaphoreHandle_t s_status_lock;

// Serializes network operations (a periodic check must not race a manual
// check or an install, and two installs must not run at once).
static SemaphoreHandle_t s_op_lock;

// ======================================================
// Status helpers
// ======================================================

static void status_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    const esp_app_desc_t *app = esp_app_get_description();
    strncpy(s_status.installed_version, app->version, sizeof(s_status.installed_version) - 1);
    s_status.state = FW_UPDATE_STATE_IDLE;
}

static void status_set_state(fw_update_state_t state)
{
    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        s_status.state = state;
        xSemaphoreGive(s_status_lock);
    }
}

static void status_set_error(const char *msg)
{
    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        s_status.state = FW_UPDATE_STATE_ERROR;
        strncpy(s_status.last_error, msg, sizeof(s_status.last_error) - 1);
        s_status.last_error[sizeof(s_status.last_error) - 1] = '\0';
        xSemaphoreGive(s_status_lock);
    }
    ESP_LOGW(TAG, "%s", msg);
}

void firmware_update_get_status(fw_update_status_t *out)
{
    if (!s_status_lock) {          // Not initialized yet
        memset(out, 0, sizeof(*out));
        return;
    }
    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        *out = s_status;
        xSemaphoreGive(s_status_lock);
    }
}

// ======================================================
// Version comparison (major.minor.patch, tolerant of a leading 'v' and any
// trailing "-<n>-g<hash>[-dirty]" suffix produced by `git describe`)
// ======================================================

static void parse_semver(const char *s, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (!s) return;
    if (*s == 'v' || *s == 'V') s++;
    int idx = 0;
    while (*s && idx < 3) {
        if (!isdigit((unsigned char)*s)) break;
        int v = 0;
        while (isdigit((unsigned char)*s)) {
            v = v * 10 + (*s - '0');
            s++;
        }
        out[idx++] = v;
        if (*s == '.') s++;
        else break;
    }
}

// Returns <0 if a<b, 0 if equal, >0 if a>b (by major.minor.patch).
static int semver_cmp(const char *a, const char *b)
{
    int va[3], vb[3];
    parse_semver(a, va);
    parse_semver(b, vb);
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    }
    return 0;
}

// ======================================================
// MQTT update-entity state
// ======================================================

void firmware_update_publish_mqtt_state(void)
{
    if (!s_status_lock) return;   // Not initialized yet

    fw_update_status_t st;
    firmware_update_get_status(&st);

    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    char topic[96];
    snprintf(topic, sizeof(topic), "pool/%s/update/state", device_id);

    // Home Assistant "update" platform reads installed_version/latest_version
    // and (optionally) in_progress/update_percentage from a JSON state payload.
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "installed_version", st.installed_version);
    // Never publish an empty/unknown latest — mirror installed so HA shows
    // "up to date" until the first successful check populates a real tag.
    cJSON_AddStringToObject(root, "latest_version",
                            st.latest_version[0] ? st.latest_version : st.installed_version);
    if (st.release_url[0]) {
        cJSON_AddStringToObject(root, "release_url", st.release_url);
    }
    cJSON_AddStringToObject(root, "title", "Pool Controller Firmware");

    bool in_progress = (st.state == FW_UPDATE_STATE_DOWNLOADING);
    cJSON_AddBoolToObject(root, "in_progress", in_progress);
    if (in_progress) {
        cJSON_AddNumberToObject(root, "update_percentage", st.progress_pct);
    } else {
        cJSON_AddNullToObject(root, "update_percentage");
    }

    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        mqtt_publish(topic, json, 1, true);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

// ======================================================
// GitHub latest-release lookup
//
// Rather than hitting the rate-limited JSON API, we GET the HTML
// ".../releases/latest" endpoint with auto-redirect disabled and read the tag
// out of the 302 "Location: .../releases/tag/<tag>" header. This is cheap on
// memory (no JSON body to buffer) and needs no authentication.
// ======================================================

typedef struct {
    char *buf;
    size_t cap;
} location_ctx_t;

static esp_err_t check_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        location_ctx_t *ctx = (location_ctx_t *)evt->user_data;
        if (ctx && evt->header_key && strcasecmp(evt->header_key, "Location") == 0) {
            strncpy(ctx->buf, evt->header_value, ctx->cap - 1);
            ctx->buf[ctx->cap - 1] = '\0';
        }
    }
    return ESP_OK;
}

// Trim trailing whitespace/quotes that could ride along a header value.
static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (isspace((unsigned char)s[n - 1]) || s[n - 1] == '"' || s[n - 1] == '/')) {
        s[--n] = '\0';
    }
}

static esp_err_t query_latest_release(char *tag_out, size_t tag_len,
                                      char *url_out, size_t url_len)
{
    char location[192] = {0};
    location_ctx_t lctx = { location, sizeof(location) };

    char url[128];
    snprintf(url, sizeof(url), "https://github.com/%s/%s/releases/latest",
             FW_UPDATE_GITHUB_OWNER, FW_UPDATE_GITHUB_REPO);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = FW_UPDATE_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .event_handler = check_http_event,
        .user_data = &lctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_http_client_set_header(client, "User-Agent", "pool-controller-esp32");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "release check request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status < 300 || status >= 400 || location[0] == '\0') {
        ESP_LOGW(TAG, "unexpected release response (HTTP %d)", status);
        return ESP_FAIL;
    }

    char *tag = strstr(location, "/tag/");
    if (!tag) {
        ESP_LOGW(TAG, "no tag in redirect: %s", location);
        return ESP_FAIL;
    }
    tag += 5;  // skip "/tag/"

    rstrip(location);
    strncpy(url_out, location, url_len - 1);
    url_out[url_len - 1] = '\0';

    strncpy(tag_out, tag, tag_len - 1);
    tag_out[tag_len - 1] = '\0';
    rstrip(tag_out);
    return ESP_OK;
}

esp_err_t firmware_update_check_now(void)
{
    if (xSemaphoreTake(s_op_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "check skipped: an update operation is already running");
        return ESP_ERR_INVALID_STATE;
    }

    status_set_state(FW_UPDATE_STATE_CHECKING);

    char tag[48] = {0};
    char rel_url[160] = {0};
    esp_err_t err = query_latest_release(tag, sizeof(tag), rel_url, sizeof(rel_url));

    if (err != ESP_OK) {
        status_set_error("GitHub release check failed");
        xSemaphoreGive(s_op_lock);
        firmware_update_publish_mqtt_state();
        return err;
    }

    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        strncpy(s_status.latest_version, tag, sizeof(s_status.latest_version) - 1);
        s_status.latest_version[sizeof(s_status.latest_version) - 1] = '\0';
        strncpy(s_status.release_url, rel_url, sizeof(s_status.release_url) - 1);
        s_status.release_url[sizeof(s_status.release_url) - 1] = '\0';
        s_status.update_available =
            (semver_cmp(s_status.installed_version, s_status.latest_version) < 0);
        s_status.checked = true;
        s_status.last_error[0] = '\0';
        s_status.state = FW_UPDATE_STATE_IDLE;
        ESP_LOGI(TAG, "installed=%s latest=%s update_available=%d",
                 s_status.installed_version, s_status.latest_version,
                 s_status.update_available);
        xSemaphoreGive(s_status_lock);
    }

    xSemaphoreGive(s_op_lock);
    firmware_update_publish_mqtt_state();
    return ESP_OK;
}

// ======================================================
// OTA install from the latest GitHub release asset
// ======================================================

static void set_progress(int pct)
{
    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        s_status.progress_pct = pct;
        xSemaphoreGive(s_status_lock);
    }
}

static void install_task(void *arg)
{
    // s_op_lock is already held by the caller (firmware_update_start_install).
    char tag[48];
    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        strncpy(tag, s_status.latest_version, sizeof(tag) - 1);
        tag[sizeof(tag) - 1] = '\0';
        s_status.progress_pct = 0;
        s_status.state = FW_UPDATE_STATE_DOWNLOADING;
        xSemaphoreGive(s_status_lock);
    }

    char asset_url[224];
    snprintf(asset_url, sizeof(asset_url),
             "https://github.com/%s/%s/releases/download/%s/%s%s.bin",
             FW_UPDATE_GITHUB_OWNER, FW_UPDATE_GITHUB_REPO, tag,
             FW_UPDATE_ASSET_PREFIX, tag);
    ESP_LOGI(TAG, "Starting OTA from %s", asset_url);

    firmware_update_publish_mqtt_state();

    esp_http_client_config_t http_cfg = {
        .url = asset_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = FW_UPDATE_OTA_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota);
    if (err != ESP_OK || ota == NULL) {
        status_set_error("Download failed to start");
        firmware_update_publish_mqtt_state();
        xSemaphoreGive(s_op_lock);
        vTaskDelete(NULL);
        return;
    }

    int total = esp_https_ota_get_image_size(ota);
    int last_published = -10;

    while (1) {
        err = esp_https_ota_perform(ota);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        int read = esp_https_ota_get_image_len_read(ota);
        int pct = (total > 0) ? (int)((int64_t)read * 100 / total) : 0;
        set_progress(pct);
        if (pct - last_published >= 5) {
            last_published = pct;
            ESP_LOGI(TAG, "OTA progress %d%% (%d/%d bytes)", pct, read, total);
            firmware_update_publish_mqtt_state();
        }
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(ota)) {
        err = esp_https_ota_finish(ota);
        if (err == ESP_OK) {
            set_progress(100);
            status_set_state(FW_UPDATE_STATE_SUCCESS);
            firmware_update_publish_mqtt_state();
            ESP_LOGI(TAG, "OTA successful, rebooting into new firmware");
            xSemaphoreGive(s_op_lock);
            vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
            esp_restart();
            return;  // not reached
        }
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            status_set_error("Downloaded image failed validation");
        } else {
            status_set_error("Failed to finalize update");
        }
    } else {
        esp_https_ota_abort(ota);
        status_set_error("Download failed or incomplete");
    }

    firmware_update_publish_mqtt_state();
    xSemaphoreGive(s_op_lock);
    vTaskDelete(NULL);
}

esp_err_t firmware_update_start_install(void)
{
    // Must have a known, newer target.
    bool have_target = false;
    if (xSemaphoreTake(s_status_lock, portMAX_DELAY) == pdTRUE) {
        have_target = s_status.update_available && s_status.latest_version[0];
        xSemaphoreGive(s_status_lock);
    }
    if (!have_target) {
        ESP_LOGW(TAG, "install requested but no newer release is known");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_op_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "install skipped: an update operation is already running");
        return ESP_ERR_INVALID_STATE;
    }

    // install_task owns s_op_lock from here and releases it (or reboots).
    if (xTaskCreate(install_task, "fw_ota", FW_UPDATE_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        status_set_error("Could not start update task");
        xSemaphoreGive(s_op_lock);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ======================================================
// Background check task
// ======================================================

static void check_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(FW_UPDATE_STARTUP_DELAY_MS));
    while (1) {
        firmware_update_check_now();
        vTaskDelay(pdMS_TO_TICKS(FW_UPDATE_CHECK_INTERVAL_MS));
    }
}

void firmware_update_init(void)
{
    s_status_lock = xSemaphoreCreateMutex();
    s_op_lock = xSemaphoreCreateMutex();
    if (!s_status_lock || !s_op_lock) {
        ESP_LOGE(TAG, "Failed to create firmware update locks");
        return;
    }
    status_init();

    if (xTaskCreate(check_task, "fw_check", FW_UPDATE_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start firmware check task");
    } else {
        ESP_LOGI(TAG, "Firmware update checker started (installed %s)",
                 s_status.installed_version);
    }
}
