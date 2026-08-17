#include "storage_logger.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LOG_QUEUE_LENGTH 4U
#define LOG_TASK_STACK_BYTES 5120U
#define LOG_ROTATE_BYTES (8U * 1024U * 1024U)
#define LOG_DIR BOARD_SD_MOUNT_POINT "/airtrack/logs"
#define LOG_GATE_TIMEOUT_MS 5000U
#define LOG_READ_GATE_TIMEOUT_MS 2000U
/* Enough distinct aircraft to hold a busy day's window without evictions. */
/* Hold a sighting this long for a callsign whose route lookup is pending, so
 * the record can carry origin/destination when the enrichment lands. */
#define ROUTE_GRACE_MS 20000U
#define SEEN_ENTRIES 128U
#define PRUNE_CHECK_BYTES (256U * 1024U)
#define VALID_TIME_EPOCH 1704067200L

static const char *TAG = "storage";

typedef struct {
    char hex[16];
    TickType_t logged_at;
    bool pending;          /* seen but not yet written (waiting for route) */
    TickType_t first_seen;
} seen_entry_t;

typedef struct {
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    QueueHandle_t queue;
    StaticQueue_t queue_storage;
    uint8_t queue_buffer[LOG_QUEUE_LENGTH * sizeof(airtrack_snapshot_t)];
    TaskHandle_t task;
    StaticTask_t task_storage;
    StackType_t task_stack[LOG_TASK_STACK_BYTES / sizeof(StackType_t)];
    bool running;
    bool stop_requested;
    bool sd_mounted;
    airtrack_settings_t settings;
    storage_logger_status_t status;
} logger_context_t;

static logger_context_t s_logger;

static void copy_settings(airtrack_settings_t *settings, bool *stop)
{
    xSemaphoreTake(s_logger.lock, portMAX_DELAY);
    *settings = s_logger.settings;
    *stop = s_logger.stop_requested;
    xSemaphoreGive(s_logger.lock);
}

static size_t escape_json(char *out, size_t capacity, const char *input)
{
    size_t used = 0U;
    for (size_t index = 0U; input[index] != '\0'; ++index) {
        const char byte = input[index];
        if (byte == '"' || byte == '\\') {
            if (used + 2U >= capacity) {
                return 0U;
            }
            out[used++] = '\\';
            out[used++] = byte;
        } else {
            if (used + 1U >= capacity) {
                return 0U;
            }
            out[used++] = byte;
        }
    }
    out[used] = '\0';
    return used;
}

/* Names: YYYY-MM-DD.ndjson, YYYY-MM-DD.ndjson.1, or unsynced.ndjson[.1]. */
bool storage_logger_valid_name(const char *name)
{
    if (name == NULL) {
        return false;
    }
    const size_t length = strnlen(name, STORAGE_LOG_NAME_MAX_BYTES + 1U);
    if (length > STORAGE_LOG_NAME_MAX_BYTES) {
        return false;
    }
    const char *rest;
    if (strncmp(name, "unsynced", 8U) == 0) {
        rest = name + 8U;
    } else {
        for (size_t index = 0U; index < 10U; ++index) {
            const char byte = name[index];
            if (index == 4U || index == 7U) {
                if (byte != '-') {
                    return false;
                }
            } else if (!isdigit((unsigned char)byte)) {
                return false;
            }
        }
        rest = name + 10U;
    }
    return strcmp(rest, ".ndjson") == 0 || strcmp(rest, ".ndjson.1") == 0;
}

static esp_err_t ensure_directories(void)
{
    if (mkdir(BOARD_SD_MOUNT_POINT "/airtrack", 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    if (mkdir(LOG_DIR, 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void make_log_name(char name[STORAGE_LOG_NAME_MAX_BYTES + 1U])
{
    const time_t now = time(NULL);
    struct tm utc;
    if (now >= VALID_TIME_EPOCH && gmtime_r(&now, &utc) != NULL) {
        (void)snprintf(name, STORAGE_LOG_NAME_MAX_BYTES + 1U,
                       "%04u-%02u-%02u.ndjson", (unsigned)(utc.tm_year + 1900) % 10000U,
                       (unsigned)(utc.tm_mon + 1) % 100U, (unsigned)utc.tm_mday % 100U);
    } else {
        memcpy(name, "unsynced.ndjson", sizeof("unsynced.ndjson"));
    }
}

static esp_err_t rotate_if_needed(const char *path)
{
    struct stat info;
    if (stat(path, &info) != 0 || info.st_size < (off_t)LOG_ROTATE_BYTES) {
        return ESP_OK;
    }
    char rotated[104];
    const int length = snprintf(rotated, sizeof(rotated), "%s.1", path);
    if (length < 0 || (size_t)length >= sizeof(rotated)) {
        return ESP_ERR_INVALID_SIZE;
    }
    (void)unlink(rotated);
    return rename(path, rotated) == 0 ? ESP_OK : ESP_FAIL;
}

/* Bounded directory scan; caller holds the SPI gate. Sorted oldest first. */
static int compare_names(const void *left, const void *right)
{
    const storage_log_file_t *a = left;
    const storage_log_file_t *b = right;
    /* "unsynced" records predate time sync: treat as oldest. */
    const bool a_unsynced = a->name[0] == 'u';
    const bool b_unsynced = b->name[0] == 'u';
    if (a_unsynced != b_unsynced) {
        return a_unsynced ? -1 : 1;
    }
    return strcmp(a->name, b->name);
}

static esp_err_t scan_logs(storage_log_file_t *files, size_t capacity,
                           size_t *count, uint64_t *total_bytes)
{
    *count = 0U;
    *total_bytes = 0U;
    DIR *directory = opendir(LOG_DIR);
    if (directory == NULL) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!storage_logger_valid_name(entry->d_name)) {
            continue;
        }
        char path[128];
        struct stat info;
        if (snprintf(path, sizeof(path), LOG_DIR "/%.31s", entry->d_name) < 0 ||
            stat(path, &info) != 0) {
            continue;
        }
        *total_bytes += (uint64_t)info.st_size;
        if (*count < capacity) {
            (void)snprintf(files[*count].name, sizeof(files[*count].name), "%.31s",
                           entry->d_name);
            files[*count].bytes = (uint32_t)info.st_size;
            ++(*count);
        }
    }
    closedir(directory);
    qsort(files, *count, sizeof(files[0]), compare_names);
    return ESP_OK;
}

/* Delete oldest files until size and day limits hold. Caller holds gate. */
static void enforce_retention(const airtrack_settings_t *settings,
                              const char *current_name)
{
    storage_log_file_t files[STORAGE_LOG_MAX_LISTED];
    size_t count = 0U;
    uint64_t total = 0U;
    if (scan_logs(files, STORAGE_LOG_MAX_LISTED, &count, &total) != ESP_OK) {
        return;
    }
    const uint64_t limit = (uint64_t)settings->retention_mib * 1024ULL * 1024ULL;
    uint32_t pruned = 0U;
    for (size_t index = 0U; index < count; ++index) {
        const bool is_current = strcmp(files[index].name, current_name) == 0;
        bool too_old = false;
        if (!is_current && files[index].name[0] != 'u' && current_name[0] != 'u') {
            /* Compare YYYY-MM-DD lexically against the cut-off date. */
            const time_t now = time(NULL);
            const time_t cutoff = now - (time_t)settings->retention_days * 86400L;
            struct tm utc;
            char cutoff_name[16];
            if (gmtime_r(&cutoff, &utc) != NULL) {
                (void)snprintf(cutoff_name, sizeof(cutoff_name), "%04u-%02u-%02u",
                               (unsigned)(utc.tm_year + 1900) % 10000U,
                               (unsigned)(utc.tm_mon + 1) % 100U,
                               (unsigned)utc.tm_mday % 100U);
                too_old = strncmp(files[index].name, cutoff_name, 10U) < 0;
            }
        }
        if (is_current || (total <= limit && !too_old)) {
            continue;
        }
        char path[128];
        (void)snprintf(path, sizeof(path), LOG_DIR "/%.31s", files[index].name);
        if (unlink(path) == 0) {
            total -= files[index].bytes;
            ++pruned;
            ESP_LOGI(TAG, "pruned %s (%s)", files[index].name,
                     too_old ? "older than retention" : "size cap");
        }
    }
    xSemaphoreTake(s_logger.lock, portMAX_DELAY);
    s_logger.status.files_pruned += pruned;
    s_logger.status.log_bytes = total;
    s_logger.status.log_files = (uint32_t)count - pruned;
    xSemaphoreGive(s_logger.lock);
}

static esp_err_t append_record(const char *name, const airtrack_snapshot_t *snapshot,
                               const airtrack_aircraft_t *aircraft,
                               const char *event, bool nearest)
{
    char path[128];
    (void)snprintf(path, sizeof(path), LOG_DIR "/%.31s", name);
    esp_err_t result = ensure_directories();
    if (result == ESP_OK) {
        result = rotate_if_needed(path);
    }
    if (result != ESP_OK) {
        return result;
    }

    char hex[40] = "";
    char callsign[48] = "";
    char registration[48] = "";
    char type[24] = "";
    if (escape_json(hex, sizeof(hex), aircraft->hex) == 0U ||
        (aircraft->callsign[0] != '\0' &&
         escape_json(callsign, sizeof(callsign), aircraft->callsign) == 0U) ||
        (aircraft->registration[0] != '\0' &&
         escape_json(registration, sizeof(registration),
                     aircraft->registration) == 0U) ||
        (aircraft->aircraft_type[0] != '\0' &&
         escape_json(type, sizeof(type), aircraft->aircraft_type) == 0U)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char timestamp[32] = "null";
    const time_t now = time(NULL);
    struct tm utc;
    if (now >= VALID_TIME_EPOCH && gmtime_r(&now, &utc) != NULL) {
        (void)snprintf(timestamp, sizeof(timestamp),
                       "\"%04u-%02u-%02uT%02u:%02u:%02uZ\"",
                       (unsigned)(utc.tm_year + 1900) % 10000U,
                       (unsigned)(utc.tm_mon + 1) % 100U,
                       (unsigned)utc.tm_mday % 100U, (unsigned)utc.tm_hour % 100U,
                       (unsigned)utc.tm_min % 100U, (unsigned)utc.tm_sec % 100U);
    }
    char route[16] = "";
    if (aircraft->route_valid) {
        (void)snprintf(route, sizeof(route), "%s-%s", aircraft->route_from,
                       aircraft->route_to);
    }
    char line[640];
    const int length = snprintf(line, sizeof(line),
        "{\"v\":2,\"ts\":%s,\"mono_ms\":%lld,\"event\":\"%s\",\"nearest\":%s,"
        "\"hex\":\"%s\",\"flight\":\"%s\",\"reg\":\"%s\",\"type\":\"%s\","
        "\"route\":\"%s\",\"dst_nm\":%.3f,\"dir_deg\":%.1f,\"alt_ft\":%ld,"
        "\"ground\":%s,\"gs_kt\":%.1f,\"track_deg\":%.1f,\"vr_fpm\":%ld,"
        "\"squawk\":\"%s\",\"emergency\":%s,\"seen_pos_s\":%.2f}\n",
        timestamp, (long long)snapshot->updated_monotonic_ms, event,
        nearest ? "true" : "false", hex, callsign, registration, type, route,
        (double)aircraft->distance_nm, (double)aircraft->bearing_deg,
        (long)aircraft->altitude_ft, aircraft->ground ? "true" : "false",
        (double)aircraft->ground_speed_kt, (double)aircraft->track_deg,
        (long)aircraft->vertical_rate_fpm, aircraft->squawk,
        aircraft->emergency ? "true" : "false", (double)aircraft->seen_pos_s);
    if (length < 0 || (size_t)length >= sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *file = fopen(path, "ab");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const bool written = fwrite(line, 1U, (size_t)length, file) == (size_t)length;
    const bool flushed = written && fflush(file) == 0 && fsync(fileno(file)) == 0;
    const int close_result = fclose(file);
    return flushed && close_result == 0 ? ESP_OK : ESP_FAIL;
}

static seen_entry_t *seen_slot(seen_entry_t *seen, const char *hex, TickType_t now,
                               TickType_t window)
{
    seen_entry_t *slot = NULL;
    for (size_t index = 0U; index < SEEN_ENTRIES; ++index) {
        if (seen[index].hex[0] != '\0' && strcmp(seen[index].hex, hex) == 0) {
            return &seen[index];
        }
        if (slot == NULL &&
            (seen[index].hex[0] == '\0' ||
             (!seen[index].pending &&
              (TickType_t)(now - seen[index].logged_at) >= window))) {
            slot = &seen[index];
        }
    }
    if (slot == NULL) {
        slot = &seen[0]; /* all busy and fresh: reuse the first, bounded */
    }
    memset(slot, 0, sizeof(*slot));
    (void)snprintf(slot->hex, sizeof(slot->hex), "%s", hex);
    slot->pending = true;
    slot->first_seen = now;
    return slot;
}

/*
 * Decide whether this aircraft gets a sighting record now.  A new aircraft
 * with a callsign but no route yet is held for ROUTE_GRACE_MS so the record
 * can include FROM/TO once the lookup completes; anything else is written on
 * first sight and then not again within the window.
 */
static bool should_log_sighting(seen_entry_t *seen,
                                const airtrack_aircraft_t *aircraft,
                                TickType_t now, uint16_t window_min)
{
    const TickType_t window = pdMS_TO_TICKS((uint32_t)window_min * 60000U);
    seen_entry_t *entry = seen_slot(seen, aircraft->hex, now, window);
    if (!entry->pending) {
        if ((TickType_t)(now - entry->logged_at) < window) {
            return false;
        }
        entry->pending = true;
        entry->first_seen = now;
    }
    const bool route_pending = aircraft->callsign[0] != '\0' &&
                               !aircraft->route_valid;
    if (route_pending &&
        (TickType_t)(now - entry->first_seen) < pdMS_TO_TICKS(ROUTE_GRACE_MS)) {
        return false;
    }
    entry->pending = false;
    entry->logged_at = now;
    return true;
}

static void logger_task(void *argument)
{
    (void)argument;
    static seen_entry_t seen[SEEN_ENTRIES];
    memset(seen, 0, sizeof(seen));
    TickType_t last_heartbeat = 0U;
    uint32_t bytes_since_prune = PRUNE_CHECK_BYTES; /* prune on first write */
    for (;;) {
        airtrack_snapshot_t snapshot;
        if (xQueueReceive(s_logger.queue, &snapshot, pdMS_TO_TICKS(500U)) != pdTRUE) {
            airtrack_settings_t settings;
            bool stop;
            copy_settings(&settings, &stop);
            if (stop) {
                break;
            }
            continue;
        }
        airtrack_settings_t settings;
        bool stop;
        copy_settings(&settings, &stop);
        if (stop) {
            break;
        }
        if (settings.logging_mode == AIRTRACK_LOGGING_OFF ||
            snapshot.aircraft_count == 0U ||
            snapshot.state != AIRTRACK_FEED_LIVE) {
            continue;
        }

        /* Decide which records this snapshot produces. */
        const TickType_t now = xTaskGetTickCount();
        bool write_sighting[AIRTRACK_MAX_AIRCRAFT] = {false};
        bool any = false;
        for (size_t index = 0U; index < snapshot.aircraft_count; ++index) {
            if (should_log_sighting(seen, &snapshot.aircraft[index], now,
                                    settings.sighting_window_min)) {
                write_sighting[index] = true;
                any = true;
            }
        }
        const bool heartbeat =
            settings.logging_mode == AIRTRACK_LOGGING_PERIODIC &&
            (TickType_t)(now - last_heartbeat) >=
                pdMS_TO_TICKS((uint32_t)settings.log_heartbeat_s * 1000U);
        if (!any && !heartbeat) {
            continue;
        }

        if (!board_spi_acquire(pdMS_TO_TICKS(LOG_GATE_TIMEOUT_MS))) {
            xSemaphoreTake(s_logger.lock, portMAX_DELAY);
            s_logger.status.last_error = ESP_ERR_TIMEOUT;
            ++s_logger.status.records_dropped;
            xSemaphoreGive(s_logger.lock);
            ESP_LOGW(TAG, "SD log write skipped: shared SPI bus timeout");
            continue;
        }
        char name[STORAGE_LOG_NAME_MAX_BYTES + 1U];
        make_log_name(name);
        esp_err_t result = ESP_OK;
        uint32_t written = 0U;
        for (size_t index = 0U; result == ESP_OK &&
             index < snapshot.aircraft_count; ++index) {
            const bool is_heartbeat_target = heartbeat && index == 0U;
            if (!write_sighting[index] && !is_heartbeat_target) {
                continue;
            }
            result = append_record(name, &snapshot, &snapshot.aircraft[index],
                                   write_sighting[index] ? "sighting" : "heartbeat",
                                   index == 0U);
            if (result == ESP_OK) {
                ++written;
                bytes_since_prune += 320U;
            }
        }
        if (result == ESP_OK && heartbeat) {
            last_heartbeat = now;
        }
        if (result == ESP_OK && bytes_since_prune >= PRUNE_CHECK_BYTES) {
            bytes_since_prune = 0U;
            enforce_retention(&settings, name);
        }
        board_spi_release();

        xSemaphoreTake(s_logger.lock, portMAX_DELAY);
        s_logger.status.last_error = result;
        s_logger.status.records_written += written;
        if (result != ESP_OK) {
            ++s_logger.status.records_dropped;
        }
        xSemaphoreGive(s_logger.lock);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "SD log write failed: %s", esp_err_to_name(result));
        }
    }
    xSemaphoreTake(s_logger.lock, portMAX_DELAY);
    s_logger.running = false;
    s_logger.status.running = false;
    s_logger.task = NULL;
    xSemaphoreGive(s_logger.lock);
    vTaskDelete(NULL);
}

static esp_err_t ensure_primitives(void)
{
    if (s_logger.lock == NULL) {
        s_logger.lock = xSemaphoreCreateMutexStatic(&s_logger.lock_storage);
    }
    if (s_logger.queue == NULL) {
        s_logger.queue = xQueueCreateStatic(
            LOG_QUEUE_LENGTH, sizeof(airtrack_snapshot_t), s_logger.queue_buffer,
            &s_logger.queue_storage);
    }
    return s_logger.lock != NULL && s_logger.queue != NULL ? ESP_OK
                                                           : ESP_ERR_NO_MEM;
}

esp_err_t storage_logger_start(bool sd_mounted,
                               const airtrack_settings_t *settings)
{
    if (settings == NULL || airtrack_settings_validate(settings) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_primitives();
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_logger.lock, portMAX_DELAY);
    if (s_logger.running) {
        xSemaphoreGive(s_logger.lock);
        return ESP_ERR_INVALID_STATE;
    }
    xQueueReset(s_logger.queue);
    s_logger.settings = *settings;
    s_logger.sd_mounted = sd_mounted;
    s_logger.stop_requested = false;
    const storage_logger_status_t previous = s_logger.status;
    s_logger.status = (storage_logger_status_t) {
        .running = true,
        .enabled = sd_mounted && settings->logging_mode != AIRTRACK_LOGGING_OFF,
        .records_written = previous.records_written,
        .files_pruned = previous.files_pruned,
        .log_bytes = previous.log_bytes,
        .log_files = previous.log_files,
    };
    s_logger.running = true;
    s_logger.task = xTaskCreateStatic(logger_task, "storage", LOG_TASK_STACK_BYTES,
        NULL, 2, s_logger.task_stack, &s_logger.task_storage);
    if (s_logger.task == NULL) {
        s_logger.running = false;
        s_logger.status.running = false;
        xSemaphoreGive(s_logger.lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_logger.lock);
    return ESP_OK;
}

esp_err_t storage_logger_update_settings(const airtrack_settings_t *settings)
{
    if (settings == NULL || airtrack_settings_validate(settings) != ESP_OK ||
        s_logger.lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_logger.lock, portMAX_DELAY);
    s_logger.settings = *settings;
    s_logger.status.enabled = s_logger.sd_mounted &&
                              settings->logging_mode != AIRTRACK_LOGGING_OFF;
    xSemaphoreGive(s_logger.lock);
    return ESP_OK;
}

esp_err_t storage_logger_submit(const airtrack_snapshot_t *snapshot)
{
    if (snapshot == NULL || s_logger.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_logger.lock, portMAX_DELAY);
    const bool running = s_logger.running;
    const bool enabled = s_logger.status.enabled;
    xSemaphoreGive(s_logger.lock);
    if (!running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!enabled) {
        return ESP_OK;
    }
    if (xQueueSend(s_logger.queue, snapshot, 0U) != pdTRUE) {
        xSemaphoreTake(s_logger.lock, portMAX_DELAY);
        ++s_logger.status.records_dropped;
        xSemaphoreGive(s_logger.lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t storage_logger_get_status(storage_logger_status_t *status)
{
    if (status == NULL || s_logger.lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_logger.lock, portMAX_DELAY);
    *status = s_logger.status;
    xSemaphoreGive(s_logger.lock);
    return ESP_OK;
}

esp_err_t storage_logger_stop(void)
{
    if (s_logger.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_logger.lock, portMAX_DELAY);
    if (!s_logger.running) {
        xSemaphoreGive(s_logger.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_logger.stop_requested = true;
    xSemaphoreGive(s_logger.lock);
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(20U));
        xSemaphoreTake(s_logger.lock, portMAX_DELAY);
        const bool running = s_logger.running;
        xSemaphoreGive(s_logger.lock);
        if (!running) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t storage_logger_list(storage_log_file_t *files, size_t capacity,
                              size_t *count, uint64_t *total_bytes)
{
    if (files == NULL || count == NULL || total_bytes == NULL || capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0U;
    *total_bytes = 0U;
    if (!board_sd_is_mounted()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!board_spi_acquire(pdMS_TO_TICKS(LOG_READ_GATE_TIMEOUT_MS))) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t result = scan_logs(files, capacity, count, total_bytes);
    board_spi_release();
    if (result == ESP_OK) {
        /* newest first for readers */
        for (size_t left = 0U, right = *count; left + 1U < right; ++left, --right) {
            const storage_log_file_t swap = files[left];
            files[left] = files[right - 1U];
            files[right - 1U] = swap;
        }
        if (s_logger.lock != NULL) {
            xSemaphoreTake(s_logger.lock, portMAX_DELAY);
            s_logger.status.log_bytes = *total_bytes;
            s_logger.status.log_files = (uint32_t)*count;
            xSemaphoreGive(s_logger.lock);
        }
    }
    return result;
}

esp_err_t storage_logger_read(const char *name, size_t offset, void *buffer,
                              size_t capacity, size_t *read_bytes,
                              size_t *file_size)
{
    if (!storage_logger_valid_name(name) || buffer == NULL ||
        read_bytes == NULL || file_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *read_bytes = 0U;
    *file_size = 0U;
    if (!board_sd_is_mounted()) {
        return ESP_ERR_NOT_FOUND;
    }
    char path[128];
    (void)snprintf(path, sizeof(path), LOG_DIR "/%.31s", name);
    if (!board_spi_acquire(pdMS_TO_TICKS(LOG_READ_GATE_TIMEOUT_MS))) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = ESP_OK;
    struct stat info;
    FILE *file = NULL;
    if (stat(path, &info) != 0) {
        result = ESP_ERR_NOT_FOUND;
    } else {
        *file_size = (size_t)info.st_size;
        file = fopen(path, "rb");
        if (file == NULL) {
            result = ESP_FAIL;
        } else if (offset < *file_size) {
            if (fseek(file, (long)offset, SEEK_SET) != 0) {
                result = ESP_FAIL;
            } else {
                *read_bytes = fread(buffer, 1U, capacity, file);
            }
        }
    }
    if (file != NULL) {
        fclose(file);
    }
    board_spi_release();
    return result;
}

esp_err_t storage_logger_clear(uint32_t *deleted_files)
{
    if (deleted_files != NULL) {
        *deleted_files = 0U;
    }
    if (!board_sd_is_mounted()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!board_spi_acquire(pdMS_TO_TICKS(LOG_GATE_TIMEOUT_MS))) {
        return ESP_ERR_TIMEOUT;
    }
    uint32_t removed = 0U;
    esp_err_t result = ESP_OK;
    /* Several passes in case more files exist than one scan can hold. */
    for (unsigned pass = 0U; pass < 8U; ++pass) {
        storage_log_file_t files[STORAGE_LOG_MAX_LISTED];
        size_t count = 0U;
        uint64_t total = 0U;
        if (scan_logs(files, STORAGE_LOG_MAX_LISTED, &count, &total) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
        if (count == 0U) {
            break;
        }
        for (size_t index = 0U; index < count; ++index) {
            char path[128];
            (void)snprintf(path, sizeof(path), LOG_DIR "/%.31s", files[index].name);
            if (unlink(path) == 0) {
                ++removed;
            } else {
                result = ESP_FAIL;
            }
        }
        if (result != ESP_OK) {
            break;
        }
    }
    board_spi_release();
    if (s_logger.lock != NULL) {
        xSemaphoreTake(s_logger.lock, portMAX_DELAY);
        s_logger.status.log_bytes = 0U;
        s_logger.status.log_files = 0U;
        s_logger.status.records_written = 0U;
        xSemaphoreGive(s_logger.lock);
    }
    if (deleted_files != NULL) {
        *deleted_files = removed;
    }
    ESP_LOGI(TAG, "cleared %lu log file(s)", (unsigned long)removed);
    return result;
}
