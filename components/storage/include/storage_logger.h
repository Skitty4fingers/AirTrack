#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airtrack_config.h"
#include "airtrack_tracker.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_LOG_NAME_MAX_BYTES 31U
#define STORAGE_LOG_MAX_LISTED 32U

typedef struct {
    bool running;
    bool enabled;
    uint32_t records_written;
    uint32_t records_dropped;
    uint32_t files_pruned;
    uint64_t log_bytes;    /* total size of files under /sd/airtrack/logs */
    uint32_t log_files;
    esp_err_t last_error;
} storage_logger_status_t;

typedef struct {
    char name[STORAGE_LOG_NAME_MAX_BYTES + 1U];
    uint32_t bytes;
} storage_log_file_t;

/*
 * Sighting log.  With logging enabled, every distinct aircraft that enters
 * the tracked set is written once per STORAGE_SIGHTING_WINDOW (30 minutes),
 * and AIRTRACK_LOGGING_PERIODIC additionally records the nearest aircraft
 * every log_heartbeat_s.  Files are `YYYY-MM-DD.ndjson` (UTC) or
 * `unsynced.ndjson` before time sync; the total is kept under
 * retention_mib and retention_days by deleting the oldest files first.
 */
esp_err_t storage_logger_start(bool sd_mounted,
                               const airtrack_settings_t *settings);
esp_err_t storage_logger_update_settings(const airtrack_settings_t *settings);
esp_err_t storage_logger_submit(const airtrack_snapshot_t *snapshot);
esp_err_t storage_logger_get_status(storage_logger_status_t *status);
esp_err_t storage_logger_stop(void);

/**
 * List log files, newest first.  Safe from any task; takes the shared SPI
 * gate for the duration of the directory scan.  Works whenever the card is
 * mounted, even if logging is off.
 */
esp_err_t storage_logger_list(storage_log_file_t *files, size_t capacity,
                              size_t *count, uint64_t *total_bytes);

/**
 * Read up to capacity bytes of a log file starting at offset.  The name must
 * be one produced by this component (validated; no paths).  *file_size gets
 * the current file length so callers can page or tail.
 */
esp_err_t storage_logger_read(const char *name, size_t offset, void *buffer,
                              size_t capacity, size_t *read_bytes,
                              size_t *file_size);

/** True when name has the exact shape of a log file this component writes. */
bool storage_logger_valid_name(const char *name);

#ifdef __cplusplus
}
#endif
