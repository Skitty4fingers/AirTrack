#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airtrack_config.h"
#include "airtrack_tracker.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STATUS_WEB_SSID_MAX_BYTES 32U
#define STATUS_WEB_IPV4_TEXT_MAX_BYTES 15U

/**
 * Values exposed by the station-LAN status page.
 *
 * The strings are validated and copied by status_web_start() and
 * status_web_update(); the caller retains ownership of their storage.
 */
typedef struct {
    const char *ssid;
    const char *ip_address;
    bool rssi_available;
    int8_t rssi_dbm;
    bool sd_mounted;
    uint32_t flash_bytes;
    uint32_t uptime_s;
    uint32_t free_heap_bytes;
    uint32_t minimum_free_heap_bytes;
    bool time_synchronized;
    const airtrack_settings_t *settings;
    const airtrack_snapshot_t *aircraft;
} status_web_snapshot_t;

typedef esp_err_t (*status_web_save_location_cb_t)(int32_t latitude_e7,
                                                   int32_t longitude_e7,
                                                   uint16_t radius_nm,
                                                   void *user_context);

/**
 * Start the HTTP server and copy its initial status snapshot.
 *
 * The server is read-only after initial provisioning.  When the copied
 * settings have no location, one CSRF-protected location form is enabled;
 * the callback must persist it and reject later changes.
 */
esp_err_t status_web_start(const status_web_snapshot_t *snapshot,
                           status_web_save_location_cb_t save_location,
                           void *user_context);

/** Atomically replace the status snapshot used by subsequent requests. */
esp_err_t status_web_update(const status_web_snapshot_t *snapshot);

/** Stop the server and clear its copied status snapshot. */
esp_err_t status_web_stop(void);

/** Return true while the station-LAN HTTP server is running. */
bool status_web_is_running(void);

#ifdef __cplusplus
}
#endif
