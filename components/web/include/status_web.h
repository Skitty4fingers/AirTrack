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
    bool sd_logging_enabled;
    uint32_t sd_records_written;
    uint64_t sd_log_bytes;
    uint32_t sd_log_files;
    uint32_t sd_files_pruned;
    uint32_t flash_bytes;
    uint32_t uptime_s;
    uint32_t free_heap_bytes;
    uint32_t minimum_free_heap_bytes;
    bool time_synchronized;
    bool night;
    int local_minutes;      /* local minutes-of-day, -1 until time is valid */
    uint32_t polls_ok;
    uint32_t polls_failed;
    uint32_t tls_connections;
    const airtrack_settings_t *settings;
    const airtrack_snapshot_t *aircraft;
} status_web_snapshot_t;

/**
 * Persist and apply a complete validated tracker/display settings record
 * posted from the LAN dashboard.  Wi-Fi credentials are never part of it.
 * Return ESP_OK only after the settings are durably stored, and overwrite
 * *settings with the record as persisted (new generation number); the server
 * reflects it immediately in later responses.
 */
typedef esp_err_t (*status_web_save_settings_cb_t)(
    airtrack_settings_t *settings, void *user_context);

/** Schedule a controlled restart requested from the dashboard. */
typedef esp_err_t (*status_web_reboot_cb_t)(void *user_context);

/**
 * Start the HTTP server and copy its initial status snapshot.
 *
 * Every mutating request is form-encoded, requires the canonical numeric
 * Host, and carries the per-start CSRF token embedded in the page.
 */
esp_err_t status_web_start(const status_web_snapshot_t *snapshot,
                           status_web_save_settings_cb_t save_settings,
                           status_web_reboot_cb_t reboot,
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
