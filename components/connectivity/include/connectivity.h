#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONNECTIVITY_SSID_MAX_BYTES 32U
#define CONNECTIVITY_PASSWORD_MAX_BYTES 63U
#define CONNECTIVITY_IPV4_TEXT_MAX_BYTES 15U
#define CONNECTIVITY_SCAN_MAX_RESULTS 16U
#define CONNECTIVITY_SOFTAP_IPV4 "192.168.4.1"

typedef enum {
    CONNECTIVITY_MODE_OFF = 0,
    CONNECTIVITY_MODE_STATION,
    CONNECTIVITY_MODE_SOFTAP,
    CONNECTIVITY_MODE_AP_STATION,
} connectivity_mode_t;

typedef struct {
    bool initialized;
    connectivity_mode_t mode;

    /* True only while the station interface owns a valid IPv4 lease. */
    bool connected;
    bool rssi_available;
    int8_t rssi_dbm;

    /*
     * The currently useful interface. A connected station takes priority;
     * otherwise an active setup AP is reported. In station-only reconnecting
     * state, ssid remains the configured station SSID and ip_address is empty.
     */
    char ssid[CONNECTIVITY_SSID_MAX_BYTES + 1U];
    char ip_address[CONNECTIVITY_IPV4_TEXT_MAX_BYTES + 1U];

    bool station_enabled;
    bool softap_enabled;
    char softap_ssid[CONNECTIVITY_SSID_MAX_BYTES + 1U];
    char softap_ip_address[CONNECTIVITY_IPV4_TEXT_MAX_BYTES + 1U];
} connectivity_status_t;

typedef struct {
    char ssid[CONNECTIVITY_SSID_MAX_BYTES + 1U];
    int8_t rssi_dbm;
    bool secured;
} connectivity_network_t;

/** Initialize esp-netif, the default event loop, and the Wi-Fi driver once. */
esp_err_t connectivity_init(void);

/**
 * Start or update a nonblocking station connection.
 *
 * An empty password selects an open network; protected-network passwords are
 * 8..63 bytes. Reconnects are initiated by the Wi-Fi event handler.
 */
esp_err_t connectivity_start_station(const char *ssid, const char *password);
esp_err_t connectivity_stop_station(void);

/**
 * Start or update the WPA2 setup AP at 192.168.4.1.
 *
 * The password must be 8..63 bytes. If station mode is already requested,
 * Wi-Fi remains in AP+STA mode.
 */
esp_err_t connectivity_start_softap(const char *ssid, const char *password);
esp_err_t connectivity_stop_softap(void);

/**
 * Perform a synchronous scan and return nearby networks by descending RSSI.
 *
 * At most the smaller of `capacity` and CONNECTIVITY_SCAN_MAX_RESULTS entries
 * is written. Duplicate SSIDs are collapsed to their strongest access point,
 * and access points which do not publish an SSID are omitted. `count` receives
 * the number of entries written and is set to zero before a scan is attempted.
 * `networks` may be NULL only when `capacity` is zero.
 *
 * An active setup AP remains running during the scan. The call can take several
 * seconds and is serialized with all other connectivity control operations.
 */
esp_err_t connectivity_scan_networks(connectivity_network_t *networks,
                                     size_t capacity, size_t *count);

/** Copy a coherent, fixed-size status snapshot. Safe from any task. */
esp_err_t connectivity_get_status(connectivity_status_t *status);

#ifdef __cplusplus
}
#endif
