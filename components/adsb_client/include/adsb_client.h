#pragma once

#include <stdbool.h>

#include "airtrack_config.h"
#include "airtrack_tracker.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the single ADS-B polling worker. It remains paused until online. */
esp_err_t adsb_client_start(const airtrack_settings_t *settings);

/** Enable or pause Internet polling without destroying the worker. */
esp_err_t adsb_client_set_online(bool online);

/** Atomically replace settings and schedule a fresh rate-limited poll. */
esp_err_t adsb_client_update_settings(const airtrack_settings_t *settings);

/** Copy the latest immutable snapshot. */
esp_err_t adsb_client_get_snapshot(airtrack_snapshot_t *snapshot);

/** Stop the worker and release its task resources. */
esp_err_t adsb_client_stop(void);

bool adsb_client_is_running(void);

#ifdef __cplusplus
}
#endif

