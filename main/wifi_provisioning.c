#include "wifi_provisioning.h"
#include "config.h"
#include "web_handlers.h"
#include "mqtt_poolclient.h"
#include "led_helper.h"
#include "dns_server.h"
#include "device_serial.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include <esp_http_server.h>
#include "mdns.h"

static const char *TAG = "WIFI_PROV";

// WiFi event bits
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_PORTAL_BIT     BIT1

// State variables
static EventGroupHandle_t s_wifi_event_group = NULL;
static bool s_provisioning_active = false;
static bool s_wifi_connected = false;
static bool s_mdns_started = false;
static httpd_handle_t s_httpd_handle = NULL;
static int s_wifi_retry_count = 0;
static int s_auth_failures = 0;
static int64_t s_offline_since_us = 0;
static TimerHandle_t s_wifi_retry_timer = NULL;
static char s_device_ip_address[16] = {0};
static char s_mdns_hostname[64] = {0};

// ======================================================
// Forward Declarations
// ======================================================

static esp_err_t start_http_server(void);
static void log_web_endpoints(const char *host);
static esp_err_t start_station(void);

// ======================================================
// WiFi Credential Management
// ======================================================

// On a mesh network (e.g. eero) several APs broadcast the same SSID. Scan all
// channels and pick the strongest at connect time, and advertise 802.11k/v so
// the mesh can steer us to a better node while connected.
static void apply_sta_roaming_config(wifi_config_t *cfg)
{
    cfg->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg->sta.rm_enabled = 1;
    cfg->sta.btm_enabled = 1;
}

esp_err_t wifi_credentials_save(const char *ssid, const char *password)
{
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    apply_sta_roaming_config(&wifi_cfg);
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
}

static bool wifi_has_credentials(void)
{
    wifi_config_t cfg = {0};
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != '\0';
}

// Disconnect reasons that mean the AP actively rejected us, rather than simply
// not being there. These are the only ones that count as evidence the stored
// credentials have gone stale.
static bool is_auth_failure(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return true;
        default:
            return false;
    }
}

// ======================================================
// WiFi Retry Timer
// ======================================================

// Backoff grows with the number of consecutive failures and then holds at the
// table's last entry, so the device keeps knocking indefinitely but cheaply.
static uint32_t backoff_delay_ms(void)
{
    static const uint32_t table[] = WIFI_RETRY_BACKOFF_MS;
    const int count = sizeof(table) / sizeof(table[0]);

    int idx = s_wifi_retry_count - 1;
    if (idx < 0) {
        idx = 0;
    } else if (idx >= count) {
        idx = count - 1;
    }
    return table[idx];
}

static void wifi_retry_timer_callback(TimerHandle_t xTimer)
{
    if (!wifi_has_credentials()) {
        return;
    }
    ESP_LOGI(TAG, "Retry timer expired, attempting reconnection (attempt %d)...",
             s_wifi_retry_count);
    esp_wifi_connect();
}

// ======================================================
// mDNS Service
// ======================================================

static void start_mdns_service(void)
{
    // Reconnects are routine now that the station retries indefinitely; the
    // services only need registering on the first one.
    if (s_mdns_started) {
        return;
    }

    // Initialize mDNS
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed (%s) — skipping mDNS", esp_err_to_name(err));
        return;
    }

    // Build unique hostname and instance names from last 3 MAC bytes
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    char hostname[32];
    char instance_name[32];
    char debug_instance_name[40];
    snprintf(hostname,            sizeof(hostname),
             "%s-%02X%02X%02X", MDNS_HOSTNAME, mac[3], mac[4], mac[5]);
    snprintf(instance_name,       sizeof(instance_name),
             "%s %02X%02X%02X", MDNS_INSTANCE_NAME, mac[3], mac[4], mac[5]);
    snprintf(debug_instance_name, sizeof(debug_instance_name),
             "%s %02X%02X%02X", MDNS_INSTANCE_DEBUG_NAME, mac[3], mac[4], mac[5]);

    // Cache hostname for use elsewhere (e.g. web UI)
    strncpy(s_mdns_hostname, hostname, sizeof(s_mdns_hostname) - 1);
    s_mdns_hostname[sizeof(s_mdns_hostname) - 1] = '\0';

    // ESP-IDF requires hostname to be set before instance name
    if (mdns_hostname_set(hostname) != ESP_OK ||
        mdns_instance_name_set(instance_name) != ESP_OK) {
        ESP_LOGW(TAG, "mDNS hostname/instance setup failed — mDNS may be unavailable");
    }

    ESP_LOGI(TAG, "mDNS started - accessible at %s.local (%s)", hostname, instance_name);

    // Shared TXT record values
    const esp_app_desc_t *app_desc = esp_app_get_description();
    char serial[DEVICE_SERIAL_LEN];
    device_get_serial(serial, sizeof(serial));

    // Advertise HTTP service
    mdns_txt_item_t http_txt[] = {
        {"id", serial},
        {"fw", app_desc->version},
    };
    if (mdns_service_add(instance_name, "_http", "_tcp", HTTP_SERVER_PORT,
                         http_txt, sizeof(http_txt) / sizeof(mdns_txt_item_t)) != ESP_OK) {
        ESP_LOGW(TAG, "mDNS: failed to register HTTP service");
    }
    ESP_LOGI(TAG, "  - HTTP service: http://%s.local:%d", hostname, HTTP_SERVER_PORT);

    // Advertise TCP bridge service (custom service type)
    mdns_txt_item_t tcp_bridge_txt[] = {
        {"protocol", "pool-controller-bus"},
        {"version",  "1.0"},
        {"id", serial},
        {"fw", app_desc->version},
    };
    if (mdns_service_add(debug_instance_name, "_pool-bridge", "_tcp", TCP_BRIDGE_PORT,
                         tcp_bridge_txt, sizeof(tcp_bridge_txt) / sizeof(mdns_txt_item_t)) != ESP_OK) {
        ESP_LOGW(TAG, "mDNS: failed to register pool-bridge service");
    }
    ESP_LOGI(TAG, "  - Pool Bridge service: tcp://%s.local:%d (%s)", hostname, TCP_BRIDGE_PORT, debug_instance_name);

    s_mdns_started = true;
}

const char *wifi_get_mdns_hostname(void)
{
    return s_mdns_hostname;
}

// ======================================================
// WiFi Event Handler
// ======================================================

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (wifi_has_credentials()) {
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "No WiFi credentials stored - rescue portal will start shortly");
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc =
            (const wifi_event_sta_disconnected_t *)event_data;
        const uint8_t reason = (disc != NULL) ? disc->reason : 0;

        s_wifi_connected = false;
        s_device_ip_address[0] = '\0';
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Deliberately do NOT stop the MQTT client here. This handler runs in
        // the system event-loop task, and esp_mqtt_client_stop() blocks waiting
        // for the MQTT task to acknowledge — if that task is itself stuck on a
        // dead socket during the same outage, stopping it here would wedge the
        // event loop and stall all further WiFi/IP event processing. esp-mqtt
        // detects the broken connection and reconnects on its own once WiFi
        // returns, firing MQTT_EVENT_DISCONNECTED (which updates the LED).

        // Timestamp the start of the outage so the supervisor can tell a router
        // reboot from a network that is genuinely gone.
        if (s_offline_since_us == 0) {
            s_offline_since_us = esp_timer_get_time();
        }

        if (is_auth_failure(reason)) {
            s_auth_failures++;
        } else {
            s_auth_failures = 0;
        }

        s_wifi_retry_count++;

        // Stored credentials are deliberately never cleared here. Failing to
        // reach an AP is not evidence that they are wrong, and wiping them
        // stranded the device in a SoftAP that nothing could recover from
        // without a physical visit. Retry forever instead; if the credentials
        // really have gone stale, the supervisor raises the rescue portal
        // while these retries continue underneath it.
        const uint32_t delay_ms = backoff_delay_ms();
        ESP_LOGW(TAG, "WiFi disconnected (reason %u, attempt %d) - retrying in %u ms",
                 (unsigned)reason, s_wifi_retry_count, (unsigned)delay_ms);

        // Changing the period also (re)starts the one-shot timer.
        if (s_wifi_retry_timer != NULL) {
            xTimerChangePeriod(s_wifi_retry_timer, pdMS_TO_TICKS(delay_ms), 0);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        // Store IP address
        snprintf(s_device_ip_address, sizeof(s_device_ip_address),
                 IPSTR, IP2STR(&event->ip_info.ip));

        ESP_LOGI(TAG, "Got IP: %s", s_device_ip_address);
        log_web_endpoints(s_device_ip_address);
        s_wifi_connected = true;
        s_wifi_retry_count = 0;
        s_auth_failures = 0;
        s_offline_since_us = 0;

        // Tearing the rescue portal down is left to the supervisor task:
        // esp_wifi_set_mode() must not be called from the event-loop task.

        // Stop retry timer if running
        if (s_wifi_retry_timer != NULL && xTimerIsTimerActive(s_wifi_retry_timer)) {
            xTimerStop(s_wifi_retry_timer, 0);
        }

        led_set_connected();
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Start the MQTT client the first time we have connectivity. This is
        // idempotent: on later reconnects esp-mqtt is already running and
        // managing its own reconnection, so this call is a no-op.
        mqtt_client_start();

        // Start mDNS service for network discovery
        start_mdns_service();
    }
}

// ======================================================
// HTTP Server
// ======================================================

// Binds to every interface, so a single instance serves the station network
// and the rescue portal alike. Started once at init, before any address
// exists — the listening socket does not need one.
static esp_err_t start_http_server(void)
{
    if (s_httpd_handle != NULL) {
        return ESP_OK;
    }

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = HTTP_SERVER_PORT;
    httpd_config.max_uri_handlers = HTTP_MAX_URI_HANDLERS;
    httpd_config.recv_wait_timeout = HTTP_RECV_TIMEOUT_SEC;
    httpd_config.send_wait_timeout = HTTP_SEND_TIMEOUT_SEC;
    httpd_config.stack_size = HTTP_STACK_SIZE;
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_httpd_handle, &httpd_config);
    if (err == ESP_OK) {
        web_handlers_register(s_httpd_handle);
        ESP_LOGI(TAG, "HTTP server listening on port %d (all interfaces)", HTTP_SERVER_PORT);
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
    }

    return err;
}

static void log_web_endpoints(const char *host)
{
    ESP_LOGI(TAG, "Web interface at http://%s", host);
    ESP_LOGI(TAG, "  - WiFi config: http://%s/", host);
    ESP_LOGI(TAG, "  - Pool status: http://%s/status", host);
    ESP_LOGI(TAG, "  - MQTT config: http://%s/mqtt_config", host);
    ESP_LOGI(TAG, "  - Firmware update: http://%s/update", host);
}

// ======================================================
// WiFi Initialization
// ======================================================

static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             WIFI_PROV_SOFTAP_SSID_PREFIX, eth_mac[3], eth_mac[4], eth_mac[5]);
}

// ======================================================
// Rescue Portal
// ======================================================
//
// The portal is a fallback, not a destination. It comes up in APSTA so the
// station keeps retrying the configured network underneath it, and the
// supervisor drops it again as soon as that succeeds — so a device that lands
// here because the outage outlasted the grace period still heals by itself,
// with no physical visit needed.
//
// Both entry points must run outside the event-loop task: switching WiFi mode
// from inside an event handler can wedge the loop that delivers the very
// events this module depends on.

static esp_err_t rescue_portal_start(void)
{
    char ap_ssid[32];
    get_device_service_name(ap_ssid, sizeof(ap_ssid));

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(ap_ssid),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };
    memcpy(ap_config.ap.ssid, ap_ssid, strlen(ap_ssid));
    memcpy(ap_config.ap.password, WIFI_PROV_SOFTAP_PASSWORD, strlen(WIFI_PROV_SOFTAP_PASSWORD));

    // Add the AP to the running station rather than restarting WiFi, so any
    // connection attempt in flight is left undisturbed.
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start rescue portal: %s", esp_err_to_name(err));
        return err;
    }

    s_provisioning_active = true;
    xEventGroupSetBits(s_wifi_event_group, WIFI_PORTAL_BIT);
    led_set_unconfigured();

    esp_err_t dns_err = dns_server_start();
    if (dns_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start DNS server: %s", esp_err_to_name(dns_err));
    } else {
        ESP_LOGI(TAG, "Captive portal DNS server started");
    }

    ESP_LOGW(TAG, "Rescue portal active - connect to '%s' and navigate to http://%s "
                  "(station keeps retrying in the background)",
             ap_ssid, WIFI_PROV_SOFTAP_IP);
    log_web_endpoints(WIFI_PROV_SOFTAP_IP);
    return ESP_OK;
}

static void rescue_portal_stop(void)
{
    ESP_LOGI(TAG, "Station connected - shutting the rescue portal down");

    dns_server_stop();

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to leave APSTA mode: %s", esp_err_to_name(err));
    }

    s_provisioning_active = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_PORTAL_BIT);
}

// With no credentials the portal is the only way in, so it goes up at once.
// Otherwise it waits out a grace period long enough to cover a router reboot
// or a power cut, shortened when the AP has actively rejected our credentials.
static bool rescue_portal_due(void)
{
    if (!wifi_has_credentials()) {
        return true;
    }
    if (s_offline_since_us == 0) {
        return false;
    }

    const int64_t offline_ms = (esp_timer_get_time() - s_offline_since_us) / 1000;
    const int64_t threshold_ms = (s_auth_failures >= WIFI_AUTH_FAIL_THRESHOLD)
                                     ? WIFI_RESCUE_PORTAL_AUTH_MS
                                     : WIFI_RESCUE_PORTAL_DELAY_MS;
    return offline_ms >= threshold_ms;
}

static void wifi_supervisor_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_SUPERVISOR_INTERVAL_MS));

        if (s_wifi_connected) {
            if (s_provisioning_active) {
                rescue_portal_stop();
            }
            continue;
        }

        if (!s_provisioning_active && rescue_portal_due()) {
            rescue_portal_start();
        }
    }
}

static esp_err_t start_station(void)
{
    wifi_config_t wifi_cfg = {0};
    esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
    if (wifi_cfg.sta.ssid[0] != '\0') {
        ESP_LOGI(TAG, "WiFi credentials found, connecting to SSID: %s", wifi_cfg.sta.ssid);
    } else {
        ESP_LOGI(TAG, "No WiFi credentials configured");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Re-apply roaming config on every boot so devices provisioned before
    // this option existed also get it.
    if (wifi_cfg.sta.ssid[0] != '\0') {
        apply_sta_roaming_config(&wifi_cfg);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

// ======================================================
// Public API
// ======================================================

esp_err_t wifi_provisioning_init(void)
{
    // Create event group
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }

    // Create WiFi retry timer (one-shot; each disconnect re-arms it with the
    // current backoff delay)
    if (s_wifi_retry_timer == NULL) {
        s_wifi_retry_timer = xTimerCreate("wifi_retry",
                                          pdMS_TO_TICKS(backoff_delay_ms()),
                                          pdFALSE,
                                          NULL,
                                          wifi_retry_timer_callback);
        if (s_wifi_retry_timer == NULL) {
            ESP_LOGE(TAG, "Failed to create WiFi retry timer");
        }
    }

    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    ESP_LOGI(TAG, "WiFi initialization complete");

    esp_err_t err = start_station();
    if (err != ESP_OK) {
        return err;
    }

    // Start the web UI up front so it is reachable the moment either interface
    // has an address, without anything having to wait on the network first.
    start_http_server();

    if (xTaskCreate(wifi_supervisor_task, "wifi_super", WIFI_SUPERVISOR_STACK,
                    NULL, WIFI_SUPERVISOR_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi supervisor task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool wifi_is_provisioning_active(void)
{
    return s_provisioning_active;
}

bool wifi_is_connected(void)
{
    return s_wifi_connected;
}

const char* wifi_get_device_ip(void)
{
    return s_device_ip_address;
}

bool wifi_wait_for_connection(uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "Waiting up to %u ms for WiFi connection...", (unsigned)timeout_ms);

    // Return early if the rescue portal comes up, since that means the wait
    // has nothing left to wait for right now — but the station carries on
    // retrying either way, so the caller is free to continue.
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_PORTAL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    return (bits & WIFI_CONNECTED_BIT) != 0;
}
