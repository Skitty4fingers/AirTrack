#include "storage_logger.h"

#include <errno.h>
#include <stdio.h>
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
#define LOG_TASK_STACK_BYTES 4096U
#define LOG_ROTATE_BYTES (8U * 1024U * 1024U)
#define LOG_DIR BOARD_SD_MOUNT_POINT "/airtrack/logs"

static const char *TAG = "storage";

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

static void make_log_path(char path[96])
{
    const time_t now = time(NULL);
    struct tm utc;
    if (now >= 1704067200L && gmtime_r(&now, &utc) != NULL) {
        (void)snprintf(path, 96, LOG_DIR "/%04d-%02d-%02d.ndjson",
                       utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    } else {
        memcpy(path, LOG_DIR "/unsynced.ndjson",
               sizeof(LOG_DIR "/unsynced.ndjson"));
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

static esp_err_t append_snapshot(const airtrack_snapshot_t *snapshot)
{
    char path[96];
    make_log_path(path);
    esp_err_t result = ensure_directories();
    if (result == ESP_OK) {
        result = rotate_if_needed(path);
    }
    if (result != ESP_OK) {
        return result;
    }

    char line[768];
    const airtrack_aircraft_t *aircraft =
        snapshot->aircraft_count > 0U ? &snapshot->aircraft[0] : NULL;
    char hex[40] = "";
    char callsign[48] = "";
    char registration[48] = "";
    if (aircraft != NULL &&
        (escape_json(hex, sizeof(hex), aircraft->hex) == 0U ||
         (aircraft->callsign[0] != '\0' &&
          escape_json(callsign, sizeof(callsign), aircraft->callsign) == 0U) ||
         (aircraft->registration[0] != '\0' &&
          escape_json(registration, sizeof(registration),
                      aircraft->registration) == 0U))) {
        return ESP_ERR_INVALID_SIZE;
    }
    const time_t now = time(NULL);
    const int length = aircraft != NULL
        ? snprintf(line, sizeof(line),
            "{\"v\":1,\"epoch\":%lld,\"mono_ms\":%lld,\"state\":\"%s\","
            "\"hex\":\"%s\",\"flight\":\"%s\",\"reg\":\"%s\","
            "\"dst_nm\":%.3f,\"dir_deg\":%.1f,\"alt_ft\":%ld,"
            "\"ground\":%s,\"gs_kt\":%.1f,\"track_deg\":%.1f,"
            "\"vr_fpm\":%ld,\"seen_pos_s\":%.2f}\n",
            (long long)(now >= 1704067200L ? now : 0),
            (long long)snapshot->updated_monotonic_ms,
            airtrack_feed_state_name(snapshot->state), hex, callsign,
            registration, (double)aircraft->distance_nm,
            (double)aircraft->bearing_deg, (long)aircraft->altitude_ft,
            aircraft->ground ? "true" : "false",
            (double)aircraft->ground_speed_kt, (double)aircraft->track_deg,
            (long)aircraft->vertical_rate_fpm, (double)aircraft->seen_pos_s)
        : snprintf(line, sizeof(line),
            "{\"v\":1,\"epoch\":%lld,\"mono_ms\":%lld,\"state\":\"%s\","
            "\"aircraft\":null}\n",
            (long long)(now >= 1704067200L ? now : 0),
            (long long)snapshot->updated_monotonic_ms,
            airtrack_feed_state_name(snapshot->state));
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

static void logger_task(void *argument)
{
    (void)argument;
    char previous_hex[16] = {0};
    airtrack_feed_state_t previous_state = AIRTRACK_FEED_CONFIG_REQUIRED;
    TickType_t last_record = 0U;
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
        const char *hex = snapshot.aircraft_count > 0U
                              ? snapshot.aircraft[0].hex : "";
        const bool changed = strcmp(hex, previous_hex) != 0 ||
                             snapshot.state != previous_state;
        const bool heartbeat = (TickType_t)(xTaskGetTickCount() - last_record) >=
            pdMS_TO_TICKS((uint32_t)settings.log_heartbeat_s * 1000U);
        const bool should_write = settings.logging_mode == AIRTRACK_LOGGING_PERIODIC
                                      ? (changed || heartbeat)
                                  : settings.logging_mode ==
                                        AIRTRACK_LOGGING_TARGET_CHANGES
                                      ? changed : false;
        if (should_write && board_spi_acquire(pdMS_TO_TICKS(5000U))) {
            const esp_err_t result = append_snapshot(&snapshot);
            board_spi_release();
            xSemaphoreTake(s_logger.lock, portMAX_DELAY);
            s_logger.status.last_error = result;
            if (result == ESP_OK) {
                ++s_logger.status.records_written;
                last_record = xTaskGetTickCount();
                (void)snprintf(previous_hex, sizeof(previous_hex), "%s", hex);
                previous_state = snapshot.state;
            }
            xSemaphoreGive(s_logger.lock);
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "SD log write failed: %s", esp_err_to_name(result));
            }
        } else if (should_write) {
            xSemaphoreTake(s_logger.lock, portMAX_DELAY);
            s_logger.status.last_error = ESP_ERR_TIMEOUT;
            ++s_logger.status.records_dropped;
            xSemaphoreGive(s_logger.lock);
            ESP_LOGW(TAG, "SD log write skipped: shared SPI bus timeout");
        }
    }
    xSemaphoreTake(s_logger.lock, portMAX_DELAY);
    s_logger.running = false;
    s_logger.status.running = false;
    s_logger.task = NULL;
    xSemaphoreGive(s_logger.lock);
    vTaskDelete(NULL);
}

esp_err_t storage_logger_start(bool sd_mounted,
                               const airtrack_settings_t *settings)
{
    if (settings == NULL || airtrack_settings_validate(settings) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_logger.lock == NULL) {
        s_logger.lock = xSemaphoreCreateMutexStatic(&s_logger.lock_storage);
    }
    if (s_logger.queue == NULL) {
        s_logger.queue = xQueueCreateStatic(
            LOG_QUEUE_LENGTH, sizeof(airtrack_snapshot_t), s_logger.queue_buffer,
            &s_logger.queue_storage);
    }
    if (s_logger.lock == NULL || s_logger.queue == NULL) {
        return ESP_ERR_NO_MEM;
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
    s_logger.status = (storage_logger_status_t) {
        .running = true,
        .enabled = sd_mounted && settings->logging_mode != AIRTRACK_LOGGING_OFF,
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
