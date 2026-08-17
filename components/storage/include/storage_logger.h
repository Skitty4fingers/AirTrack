#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airtrack_config.h"
#include "airtrack_tracker.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool running;
    bool enabled;
    uint32_t records_written;
    uint32_t records_dropped;
    esp_err_t last_error;
} storage_logger_status_t;

esp_err_t storage_logger_start(bool sd_mounted,
                               const airtrack_settings_t *settings);
esp_err_t storage_logger_update_settings(const airtrack_settings_t *settings);
esp_err_t storage_logger_submit(const airtrack_snapshot_t *snapshot);
esp_err_t storage_logger_get_status(storage_logger_status_t *status);
esp_err_t storage_logger_stop(void);

#ifdef __cplusplus
}
#endif

