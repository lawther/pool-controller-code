#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <esp_http_server.h>

// All provisioning constants are defined in config.h

/**
 * Initialize WiFi, start the HTTP server and start the WiFi supervisor.
 *
 * Returns as soon as the station has been started — it does not wait for a
 * connection. If credentials exist the station retries them indefinitely with
 * a capped backoff; credentials are never cleared automatically. The
 * supervisor raises the rescue portal if there are no credentials at all, or
 * if the station stays offline past the grace period in config.h.
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_provisioning_init(void);

/**
 * Save WiFi credentials via the WiFi driver (persisted to flash automatically)
 *
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @return ESP_OK on success
 */
esp_err_t wifi_credentials_save(const char *ssid, const char *password);

/**
 * Check if the rescue portal is up
 *
 * The portal runs alongside a station that is still retrying the configured
 * network, and is taken down automatically once that connection succeeds.
 *
 * @return true if the rescue portal (SoftAP) is active
 */
bool wifi_is_provisioning_active(void);

/**
 * Check if WiFi is connected
 *
 * @return true if connected to WiFi
 */
bool wifi_is_connected(void);

/**
 * Get current device IP address
 *
 * @return IP address string (e.g., "192.168.0.123") or empty string if not connected
 */
const char* wifi_get_device_ip(void);

/**
 * Get the resolved mDNS hostname (e.g. "poolcontrol" or "poolcontrol-2")
 * Returns an empty string before mDNS has started.
 *
 * @return hostname string without the ".local" suffix
 */
const char* wifi_get_mdns_hostname(void);

/**
 * Wait up to timeout_ms for a WiFi connection.
 *
 * Bounded on purpose: the bus bridge and the rest of the device must start
 * whether or not the network is available. Returns early if the rescue portal
 * comes up. The station keeps retrying in the background regardless of the
 * result, so a false return is not terminal.
 *
 * @param timeout_ms maximum time to wait, in milliseconds
 * @return true if WiFi connected within the timeout
 */
bool wifi_wait_for_connection(uint32_t timeout_ms);

/**
 * Register HTTP server handle for web interface
 * Should be called after wifi_provisioning_init()
 *
 * @param server HTTP server handle
 * @return ESP_OK on success
 */
esp_err_t wifi_register_http_server(httpd_handle_t server);

#endif // WIFI_PROVISIONING_H
