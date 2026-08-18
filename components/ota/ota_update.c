#include "ota_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#define OTA_TASK_STACK_BYTES (9U * 1024U)
#define OTA_MANIFEST_MAX_BYTES 4096U
#define OTA_MANIFEST_TIMEOUT_MS 15000
#define OTA_DOWNLOAD_TIMEOUT_MS 20000
#define OTA_READ_CHUNK_BYTES 4096U
#define OTA_MIN_IMAGE_BYTES (256U * 1024U)
#define OTA_RESTART_DELAY_MS 2500U

static const char *TAG = "ota";

typedef struct {
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    char manifest_url[OTA_URL_MAX_BYTES + 1U];
    char image_url[OTA_URL_MAX_BYTES + 1U];
    uint8_t sha256[32];
    bool have_manifest;
    ota_prepare_cb_t prepare;
    void *user_context;
    bool busy;
    bool install_requested;
    ota_status_t status;
} ota_context_t;

static ota_context_t s_ota;

typedef struct {
    char *body;
    size_t length;
    bool overflow;
} manifest_response_t;

static void set_state(ota_state_t state, const char *error)
{
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.status.state = state;
    if (error != NULL) {
        (void)snprintf(s_ota.status.error, sizeof(s_ota.status.error), "%s", error);
    } else if (state != OTA_STATE_FAILED) {
        s_ota.status.error[0] = '\0';
    }
    xSemaphoreGive(s_ota.lock);
}

int ota_compare_versions(const char *left, const char *right)
{
    for (unsigned part = 0U; part < 3U; ++part) {
        const long l = strtol(left, (char **)&left, 10);
        const long r = strtol(right, (char **)&right, 10);
        if (l != r) {
            return l < r ? -1 : 1;
        }
        if (*left == '.') {
            ++left;
        }
        if (*right == '.') {
            ++right;
        }
    }
    return 0;
}

static esp_err_t manifest_event(esp_http_client_event_t *event)
{
    manifest_response_t *response = event != NULL ? event->user_data : NULL;
    if (response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if (response->length + (size_t)event->data_len > OTA_MANIFEST_MAX_BYTES) {
            response->overflow = true;
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(response->body + response->length, event->data, (size_t)event->data_len);
        response->length += (size_t)event->data_len;
        response->body[response->length] = '\0';
    }
    return ESP_OK;
}

static bool copy_json_string(const cJSON *item, char *out, size_t capacity)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    (void)snprintf(out, capacity, "%s", item->valuestring);
    return true;
}

static bool parse_hex_sha256(const char *text, uint8_t out[32])
{
    if (text == NULL || strlen(text) != 64U) {
        return false;
    }
    for (size_t index = 0U; index < 32U; ++index) {
        unsigned value = 0U;
        for (unsigned nibble = 0U; nibble < 2U; ++nibble) {
            const char c = text[index * 2U + nibble];
            value <<= 4U;
            if (c >= '0' && c <= '9') {
                value |= (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= (unsigned)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= (unsigned)(c - 'A' + 10);
            } else {
                return false;
            }
        }
        out[index] = (uint8_t)value;
    }
    return true;
}

static bool allowed_https_url(const char *url)
{
    /* Only GitHub-owned hosts over HTTPS; the release asset redirect lands on
     * objects.githubusercontent.com. */
    static const char *hosts[] = {
        "https://github.com/", "https://skitty4fingers.github.io/",
        "https://objects.githubusercontent.com/", "https://raw.githubusercontent.com/",
        "https://release-assets.githubusercontent.com/",
    };
    for (size_t index = 0U; index < sizeof(hosts) / sizeof(hosts[0]); ++index) {
        if (strncmp(url, hosts[index], strlen(hosts[index])) == 0) {
            return true;
        }
    }
    return false;
}

static void run_check(void)
{
    set_state(OTA_STATE_CHECKING, NULL);
    manifest_response_t response = {
        .body = calloc(1U, OTA_MANIFEST_MAX_BYTES + 1U),
    };
    if (response.body == NULL) {
        set_state(OTA_STATE_FAILED, "low memory");
        return;
    }
    const esp_http_client_config_t config = {
        .url = s_ota.manifest_url,
        .event_handler = manifest_event,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_MANIFEST_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_agent = "AirTrack-OTA (ESP32-C6)",
        .max_redirection_count = 2,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t result = client != NULL ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
    const int http_status = client != NULL ? esp_http_client_get_status_code(client) : 0;
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    if (result != ESP_OK || http_status != 200 || response.overflow) {
        char error[OTA_ERROR_MAX_BYTES + 1U];
        (void)snprintf(error, sizeof(error), "manifest: %s (HTTP %d)",
                       esp_err_to_name(result), http_status);
        free(response.body);
        set_state(OTA_STATE_FAILED, error);
        return;
    }

    cJSON *root = cJSON_ParseWithLength(response.body, response.length);
    free(response.body);
    char version[OTA_VERSION_MAX_BYTES + 1U] = "";
    char url[OTA_URL_MAX_BYTES + 1U] = "";
    char notes[OTA_NOTES_MAX_BYTES + 1U] = "";
    char sha_text[80] = "";
    uint8_t sha[32];
    const cJSON *size_item = cJSON_GetObjectItemCaseSensitive(root, "size");
    const bool ok = cJSON_IsObject(root) &&
        copy_json_string(cJSON_GetObjectItemCaseSensitive(root, "version"), version, sizeof(version)) &&
        copy_json_string(cJSON_GetObjectItemCaseSensitive(root, "url"), url, sizeof(url)) &&
        copy_json_string(cJSON_GetObjectItemCaseSensitive(root, "sha256"), sha_text, sizeof(sha_text)) &&
        parse_hex_sha256(sha_text, sha) && cJSON_IsNumber(size_item) &&
        size_item->valuedouble >= OTA_MIN_IMAGE_BYTES && allowed_https_url(url);
    (void)copy_json_string(cJSON_GetObjectItemCaseSensitive(root, "notes"), notes, sizeof(notes));
    const uint32_t size = ok ? (uint32_t)size_item->valuedouble : 0U;
    cJSON_Delete(root);
    if (!ok) {
        set_state(OTA_STATE_FAILED, "manifest: unexpected content");
        return;
    }

    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    (void)snprintf(s_ota.image_url, sizeof(s_ota.image_url), "%s", url);
    memcpy(s_ota.sha256, sha, sizeof(sha));
    s_ota.have_manifest = true;
    (void)snprintf(s_ota.status.available_version, sizeof(s_ota.status.available_version), "%s", version);
    (void)snprintf(s_ota.status.notes, sizeof(s_ota.status.notes), "%s", notes);
    s_ota.status.size = size;
    s_ota.status.checked_monotonic_ms = esp_timer_get_time() / 1000LL;
    const bool newer = ota_compare_versions(version, s_ota.status.current_version) > 0;
    s_ota.status.state = newer ? OTA_STATE_AVAILABLE : OTA_STATE_UP_TO_DATE;
    s_ota.status.error[0] = '\0';
    xSemaphoreGive(s_ota.lock);
    ESP_LOGI(TAG, "manifest: %s (%lu bytes) — %s", version, (unsigned long)size,
             newer ? "update available" : "up to date");
}

static void run_install(void)
{
    char url[OTA_URL_MAX_BYTES + 1U];
    uint8_t expected_sha[32];
    uint32_t expected_size;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    memcpy(url, s_ota.image_url, sizeof(url));
    memcpy(expected_sha, s_ota.sha256, sizeof(expected_sha));
    expected_size = s_ota.status.size;
    s_ota.status.downloaded = 0U;
    s_ota.status.percent = 0U;
    xSemaphoreGive(s_ota.lock);

    if (s_ota.prepare != NULL) {
        s_ota.prepare(s_ota.user_context);
        vTaskDelay(pdMS_TO_TICKS(1500)); /* let the ADS-B session close */
    }
    set_state(OTA_STATE_DOWNLOADING, NULL);

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL || target->size < expected_size) {
        set_state(OTA_STATE_FAILED, "no OTA slot large enough");
        return;
    }
    uint8_t *buffer = malloc(OTA_READ_CHUNK_BYTES);
    if (buffer == NULL) {
        set_state(OTA_STATE_FAILED, "low memory");
        return;
    }
    const esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_DOWNLOAD_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .user_agent = "AirTrack-OTA (ESP32-C6)",
        .max_redirection_count = 3,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_ota_handle_t handle = 0;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    char error[OTA_ERROR_MAX_BYTES + 1U] = "";
    esp_err_t result = client != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    int64_t content_length = 0;
    int http_status = 0;
    /* GitHub release assets redirect once to a storage host; the streaming
     * API does not follow redirects itself, so do it here (bounded). */
    for (unsigned hop = 0U; result == ESP_OK && hop < 4U; ++hop) {
        result = esp_http_client_open(client, 0);
        if (result != ESP_OK) {
            (void)snprintf(error, sizeof(error), "connect: %s", esp_err_to_name(result));
            break;
        }
        content_length = esp_http_client_fetch_headers(client);
        http_status = esp_http_client_get_status_code(client);
        if (http_status == 301 || http_status == 302 || http_status == 303 ||
            http_status == 307 || http_status == 308) {
            result = esp_http_client_set_redirection(client);
            (void)esp_http_client_close(client);
            if (result != ESP_OK) {
                (void)snprintf(error, sizeof(error), "redirect without location");
            }
            continue;
        }
        break;
    }
    if (result != ESP_OK) {
        goto done;
    }
    if (http_status != 200) {
        (void)snprintf(error, sizeof(error), "download: HTTP %d", http_status);
        result = ESP_FAIL;
        goto done;
    }
    if (content_length > 0 && (uint32_t)content_length != expected_size) {
        (void)snprintf(error, sizeof(error), "size mismatch (%lld)", (long long)content_length);
        result = ESP_FAIL;
        goto done;
    }
    result = esp_ota_begin(target, expected_size, &handle);
    if (result != ESP_OK) {
        (void)snprintf(error, sizeof(error), "ota_begin: %s", esp_err_to_name(result));
        goto done;
    }
    mbedtls_sha256_starts(&sha, 0);
    uint32_t received = 0U;
    for (;;) {
        const int count = esp_http_client_read(client, (char *)buffer, OTA_READ_CHUNK_BYTES);
        if (count < 0) {
            (void)snprintf(error, sizeof(error), "read failed");
            result = ESP_FAIL;
            break;
        }
        if (count == 0) {
            break;
        }
        if (received + (uint32_t)count > expected_size) {
            (void)snprintf(error, sizeof(error), "image larger than manifest");
            result = ESP_FAIL;
            break;
        }
        result = esp_ota_write(handle, buffer, (size_t)count);
        if (result != ESP_OK) {
            (void)snprintf(error, sizeof(error), "flash write: %s", esp_err_to_name(result));
            break;
        }
        mbedtls_sha256_update(&sha, buffer, (size_t)count);
        received += (uint32_t)count;
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        s_ota.status.downloaded = received;
        s_ota.status.percent = (uint8_t)((uint64_t)received * 100U / expected_size);
        xSemaphoreGive(s_ota.lock);
    }
    if (result == ESP_OK && received != expected_size) {
        (void)snprintf(error, sizeof(error), "short download (%lu)", (unsigned long)received);
        result = ESP_FAIL;
    }
    if (result == ESP_OK) {
        set_state(OTA_STATE_VERIFYING, NULL);
        uint8_t digest[32];
        mbedtls_sha256_finish(&sha, digest);
        if (memcmp(digest, expected_sha, sizeof(digest)) != 0) {
            (void)snprintf(error, sizeof(error), "SHA-256 mismatch");
            result = ESP_FAIL;
        }
    }
    if (result == ESP_OK) {
        result = esp_ota_end(handle);
        handle = 0;
        if (result != ESP_OK) {
            (void)snprintf(error, sizeof(error), "image invalid: %s", esp_err_to_name(result));
        }
    }
    if (result == ESP_OK) {
        result = esp_ota_set_boot_partition(target);
        if (result != ESP_OK) {
            (void)snprintf(error, sizeof(error), "boot switch: %s", esp_err_to_name(result));
        }
    }
done:
    mbedtls_sha256_free(&sha);
    if (handle != 0) {
        (void)esp_ota_abort(handle);
    }
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    free(buffer);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "update failed: %s", error);
        set_state(OTA_STATE_FAILED, error);
        return;
    }
    ESP_LOGI(TAG, "update written to %s; restarting", target->label);
    set_state(OTA_STATE_READY, NULL);
    vTaskDelay(pdMS_TO_TICKS(OTA_RESTART_DELAY_MS));
    esp_restart();
}

static void ota_task(void *argument)
{
    (void)argument;
    bool install;
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    install = s_ota.install_requested;
    s_ota.install_requested = false;
    xSemaphoreGive(s_ota.lock);
    if (install) {
        run_install();
    } else {
        run_check();
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    s_ota.busy = false;
    xSemaphoreGive(s_ota.lock);
    vTaskDelete(NULL);
}

static esp_err_t launch(bool install)
{
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    if (s_ota.busy) {
        xSemaphoreGive(s_ota.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ota.busy = true;
    s_ota.install_requested = install;
    xSemaphoreGive(s_ota.lock);
    if (xTaskCreate(ota_task, "ota", OTA_TASK_STACK_BYTES, NULL, 4, NULL) != pdPASS) {
        xSemaphoreTake(s_ota.lock, portMAX_DELAY);
        s_ota.busy = false;
        xSemaphoreGive(s_ota.lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_init(const char *manifest_url, ota_prepare_cb_t prepare, void *user_context)
{
    if (manifest_url == NULL || strlen(manifest_url) > OTA_URL_MAX_BYTES ||
        !allowed_https_url(manifest_url)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ota.lock == NULL) {
        s_ota.lock = xSemaphoreCreateMutexStatic(&s_ota.lock_storage);
    }
    (void)snprintf(s_ota.manifest_url, sizeof(s_ota.manifest_url), "%s", manifest_url);
    s_ota.prepare = prepare;
    s_ota.user_context = user_context;
    const esp_app_desc_t *description = esp_app_get_description();
    (void)snprintf(s_ota.status.current_version, sizeof(s_ota.status.current_version), "%s",
                   description != NULL ? description->version : "unknown");
    const esp_partition_t *running = esp_ota_get_running_partition();
    (void)snprintf(s_ota.status.running_partition, sizeof(s_ota.status.running_partition), "%s",
                   running != NULL ? running->label : "?");
    esp_ota_img_states_t image_state;
    s_ota.status.pending_verify = running != NULL &&
        esp_ota_get_state_partition(running, &image_state) == ESP_OK &&
        image_state == ESP_OTA_IMG_PENDING_VERIFY;
    s_ota.status.state = OTA_STATE_IDLE;
    ESP_LOGI(TAG, "running %s from %s%s", s_ota.status.current_version,
             s_ota.status.running_partition,
             s_ota.status.pending_verify ? " (pending verification)" : "");
    return ESP_OK;
}

esp_err_t ota_check_async(void)
{
    if (s_ota.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return launch(false);
}

esp_err_t ota_start(const char *version)
{
    if (s_ota.lock == NULL || version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    const bool ok = s_ota.have_manifest && s_ota.status.state == OTA_STATE_AVAILABLE &&
                    strcmp(version, s_ota.status.available_version) == 0;
    xSemaphoreGive(s_ota.lock);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }
    return launch(true);
}

esp_err_t ota_get_status(ota_status_t *status)
{
    if (status == NULL || s_ota.lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    *status = s_ota.status;
    xSemaphoreGive(s_ota.lock);
    return ESP_OK;
}

bool ota_busy(void)
{
    if (s_ota.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    const bool busy = s_ota.busy;
    xSemaphoreGive(s_ota.lock);
    return busy;
}

bool ota_pending_verify(void)
{
    return s_ota.status.pending_verify;
}

esp_err_t ota_confirm_running_image(void)
{
    if (!s_ota.status.pending_verify) {
        return ESP_OK;
    }
    const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    if (result == ESP_OK) {
        s_ota.status.pending_verify = false;
        ESP_LOGI(TAG, "running image marked valid");
    }
    return result;
}

esp_err_t ota_reject_running_image_and_reboot(void)
{
    ESP_LOGE(TAG, "self-test failed; rolling back");
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}
