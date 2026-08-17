#include "adsb_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define WORKER_STACK_BYTES (11U * 1024U)
#define HTTP_TIMEOUT_MS 15000
#define MINIMUM_REQUEST_INTERVAL_MS 1100U
#define MAX_NORMAL_BACKOFF_S 300U
#define MAX_RATE_BACKOFF_S 900U
#define VALID_TIME_EPOCH 1704067200L
#define URL_MAX_BYTES 192U

static const char *TAG = "adsb_client";

typedef struct {
    airtrack_stream_parser_t *parser;
    esp_err_t parse_result;
    uint32_t retry_after_s;
} response_context_t;

typedef struct {
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    TaskHandle_t task;
    StaticTask_t task_storage;
    StackType_t task_stack[WORKER_STACK_BYTES / sizeof(StackType_t)];
    bool running;
    bool stop_requested;
    bool online;
    airtrack_settings_t settings;
    airtrack_snapshot_t snapshot;
    char pending_hex[16];
    uint8_t pending_polls;
    uint32_t polls_ok;
    uint32_t polls_failed;
    uint32_t connections;
} adsb_context_t;

static adsb_context_t s_client;

/*
 * The HTTP client is owned exclusively by the worker task and kept alive
 * between polls so that a healthy feed reuses one TLS session instead of
 * paying a full certificate-bundle handshake every few seconds. It is
 * destroyed after any transport, protocol, or parse failure and rebuilt on
 * the next attempt.
 */
static esp_http_client_handle_t s_http;
static char s_http_url[URL_MAX_BYTES];
static response_context_t s_response;

_Static_assert(WORKER_STACK_BYTES % sizeof(StackType_t) == 0U,
               "ADS-B task stack must align to StackType_t");

static int64_t monotonic_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static bool system_time_valid(void)
{
    return time(NULL) >= VALID_TIME_EPOCH;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_context_t *context = event != NULL ? event->user_data : NULL;
    if (context == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0 &&
        context->parse_result == ESP_OK && context->parser != NULL) {
        context->parse_result = airtrack_stream_parser_feed(
            context->parser, event->data, (size_t)event->data_len);
        return context->parse_result;
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        event->header_key != NULL && event->header_value != NULL &&
        strcasecmp(event->header_key, "Retry-After") == 0) {
        errno = 0;
        char *end = NULL;
        const unsigned long seconds = strtoul(event->header_value, &end, 10);
        while (end != NULL && (*end == ' ' || *end == '\t')) {
            ++end;
        }
        if (errno == 0 && end != event->header_value && end != NULL &&
            *end == '\0' && seconds > 0UL) {
            context->retry_after_s = seconds > MAX_RATE_BACKOFF_S
                                         ? MAX_RATE_BACKOFF_S
                                         : (uint32_t)seconds;
        }
    }
    return ESP_OK;
}

static void publish_snapshot(const airtrack_snapshot_t *snapshot)
{
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    s_client.snapshot = *snapshot;
    xSemaphoreGive(s_client.lock);
}

static airtrack_snapshot_t current_snapshot(void)
{
    airtrack_snapshot_t snapshot;
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    snapshot = s_client.snapshot;
    xSemaphoreGive(s_client.lock);
    return snapshot;
}

/*
 * Publish a non-poll state (offline, config required, time sync).  When a
 * previous target is still held it is shown as STALE so the last known
 * aircraft stays visible with an honest age.  Repeated calls with an unchanged
 * outcome do not bump the sequence, so consumers are not woken needlessly.
 */
static void publish_waiting(airtrack_feed_state_t state,
                            airtrack_feed_error_t error)
{
    airtrack_snapshot_t snapshot = current_snapshot();
    const airtrack_feed_state_t effective =
        snapshot.aircraft_count > 0U ? AIRTRACK_FEED_STALE : state;
    if (snapshot.state == effective && snapshot.error == error) {
        return;
    }
    snapshot.sequence++;
    snapshot.updated_monotonic_ms = monotonic_ms();
    snapshot.state = effective;
    snapshot.error = error;
    snapshot.http_status = 0;
    snapshot.retry_after_s = 0U;
    publish_snapshot(&snapshot);
}

/*
 * Announce that polling is about to start only when leaving a state in which
 * no request could be made.  A routine poll after a good response must not
 * touch the published state, otherwise the display flickers LIVE -> STALE ->
 * LIVE for the duration of every request.
 */
static void publish_searching_on_transition(void)
{
    airtrack_snapshot_t snapshot = current_snapshot();
    const bool blocked_before =
        snapshot.state == AIRTRACK_FEED_CONFIG_REQUIRED ||
        snapshot.state == AIRTRACK_FEED_TIME_SYNC ||
        (snapshot.state == AIRTRACK_FEED_OFFLINE &&
         snapshot.error == AIRTRACK_ERROR_WIFI) ||
        (snapshot.state == AIRTRACK_FEED_STALE &&
         snapshot.error == AIRTRACK_ERROR_WIFI);
    if (!blocked_before) {
        return;
    }
    if (snapshot.aircraft_count > 0U) {
        /* Keep the retained target visible; only clear the Wi-Fi error. */
        snapshot.error = AIRTRACK_ERROR_NONE;
    } else {
        snapshot.state = AIRTRACK_FEED_SEARCHING;
        snapshot.error = AIRTRACK_ERROR_NONE;
    }
    snapshot.sequence++;
    snapshot.updated_monotonic_ms = monotonic_ms();
    snapshot.http_status = 0;
    publish_snapshot(&snapshot);
}

static void publish_failure(airtrack_feed_error_t error, int http_status,
                            uint32_t retry_s)
{
    airtrack_snapshot_t snapshot = current_snapshot();
    snapshot.sequence++;
    snapshot.updated_monotonic_ms = monotonic_ms();
    snapshot.state = snapshot.aircraft_count > 0U ? AIRTRACK_FEED_STALE
                                                  : AIRTRACK_FEED_OFFLINE;
    snapshot.error = error;
    snapshot.http_status = http_status;
    snapshot.retry_after_s = retry_s;
    publish_snapshot(&snapshot);
}

static uint32_t jittered_delay_s(uint32_t base)
{
    if (base < 2U) {
        return base;
    }
    const uint32_t spread = base / 5U;
    const uint32_t sample = esp_random() % ((spread * 2U) + 1U);
    return base - spread + sample;
}

static bool wait_or_stop(uint32_t milliseconds)
{
    const uint32_t notified = ulTaskNotifyTake(pdTRUE,
        milliseconds == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(milliseconds));
    (void)notified;
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    const bool stop = s_client.stop_requested;
    xSemaphoreGive(s_client.lock);
    return stop;
}

static void drop_http_client(void)
{
    if (s_http != NULL) {
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    s_http_url[0] = '\0';
}

static esp_err_t ensure_http_client(const char *url, bool *fresh)
{
    *fresh = false;
    if (s_http != NULL && strcmp(url, s_http_url) == 0) {
        return ESP_OK;
    }
    drop_http_client();

    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = &s_response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .user_agent = "AirTrack/1.1 (ESP32-C6; personal non-commercial)",
        .keep_alive_enable = true,
        /* The production endpoint is canonical. Refuse redirects rather than
         * allowing an untrusted Location header to select another host. */
        .disable_auto_redirect = true,
    };
    s_http = esp_http_client_init(&config);
    if (s_http == NULL) {
        return ESP_ERR_NO_MEM;
    }
    (void)esp_http_client_set_header(s_http, "Accept", "application/json");
    (void)esp_http_client_set_header(s_http, "Accept-Encoding", "identity");
    (void)snprintf(s_http_url, sizeof(s_http_url), "%s", url);
    *fresh = true;
    return ESP_OK;
}

static esp_err_t perform_poll(const airtrack_settings_t *settings,
                              airtrack_snapshot_t *result, int *http_status,
                              uint32_t *retry_after_s, bool *reused)
{
    *retry_after_s = 0U;
    *http_status = 0;
    *reused = false;
    char url[URL_MAX_BYTES];
    const int url_length = snprintf(
        url, sizeof(url),
        "https://opendata.adsb.fi/api/v3/lat/%.6f/lon/%.6f/dist/%u",
        (double)settings->latitude_e7 / 10000000.0,
        (double)settings->longitude_e7 / 10000000.0,
        (unsigned)settings->radius_nm);
    if (url_length < 0 || (size_t)url_length >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    bool fresh = false;
    esp_err_t status = ensure_http_client(url, &fresh);
    if (status != ESP_OK) {
        return status;
    }
    *reused = !fresh;
    if (fresh) {
        ++s_client.connections;
    }

    airtrack_stream_parser_t *parser =
        airtrack_stream_parser_create(settings);
    if (parser == NULL) {
        drop_http_client();
        return ESP_ERR_NO_MEM;
    }
    s_response = (response_context_t) {
        .parser = parser,
        .parse_result = ESP_OK,
    };

    status = esp_http_client_perform(s_http);
    *http_status = esp_http_client_get_status_code(s_http);
    *retry_after_s = s_response.retry_after_s;
    if (status == ESP_OK && s_response.parse_result != ESP_OK) {
        status = s_response.parse_result;
    }
    if (status == ESP_OK && *http_status == 200) {
        status = airtrack_stream_parser_finish(parser, result);
    }
    s_response.parser = NULL;
    airtrack_stream_parser_destroy(parser);

    /* Only a completely healthy exchange earns connection reuse. */
    if (status != ESP_OK || *http_status != 200) {
        drop_http_client();
    }
    return status;
}

static void worker(void *argument)
{
    (void)argument;
    uint32_t failure_backoff_s = 5U;
    int64_t previous_request_ms = 0;

    for (;;) {
        xSemaphoreTake(s_client.lock, portMAX_DELAY);
        const bool stop = s_client.stop_requested;
        const bool online = s_client.online;
        const airtrack_settings_t settings = s_client.settings;
        xSemaphoreGive(s_client.lock);
        if (stop) {
            break;
        }
        if (!settings.location_configured) {
            drop_http_client();
            publish_waiting(AIRTRACK_FEED_CONFIG_REQUIRED, AIRTRACK_ERROR_CONFIG);
            if (wait_or_stop(1000U)) {
                break;
            }
            continue;
        }
        if (!online) {
            drop_http_client();
            publish_waiting(AIRTRACK_FEED_OFFLINE, AIRTRACK_ERROR_WIFI);
            if (wait_or_stop(1000U)) {
                break;
            }
            continue;
        }
        if (!system_time_valid()) {
            publish_waiting(AIRTRACK_FEED_TIME_SYNC, AIRTRACK_ERROR_TIME);
            if (wait_or_stop(1000U)) {
                break;
            }
            continue;
        }

        publish_searching_on_transition();

        airtrack_snapshot_t candidate;
        int http_status = 0;
        uint32_t retry_after_s = 0U;
        esp_err_t poll_result = ESP_FAIL;
        bool stop_now = false;
        /* A reused keep-alive connection may have been closed by the server
         * while idle; that costs one transparent retry, not a backoff. */
        for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
            const int64_t since_request = monotonic_ms() - previous_request_ms;
            if (since_request >= 0 &&
                since_request < MINIMUM_REQUEST_INTERVAL_MS &&
                wait_or_stop((uint32_t)(MINIMUM_REQUEST_INTERVAL_MS -
                                        since_request))) {
                stop_now = true;
                break;
            }
            previous_request_ms = monotonic_ms();
            bool reused = false;
            poll_result = perform_poll(&settings, &candidate, &http_status,
                                       &retry_after_s, &reused);
            const bool transport_failure =
                poll_result != ESP_OK && http_status == 0;
            if (!(reused && transport_failure)) {
                break;
            }
            ESP_LOGD(TAG, "Keep-alive connection lost (%s); reconnecting",
                     esp_err_to_name(poll_result));
        }
        if (stop_now) {
            break;
        }

        uint32_t next_delay_s = settings.poll_interval_s;
        if (poll_result == ESP_OK && http_status == 200) {
            airtrack_snapshot_t previous = current_snapshot();
            candidate.sequence = previous.sequence + 1U;
            candidate.http_status = 200;
            candidate.updated_monotonic_ms = monotonic_ms();
            candidate.last_success_monotonic_ms = candidate.updated_monotonic_ms;
            candidate.error = AIRTRACK_ERROR_NONE;
            airtrack_apply_target_hysteresis(&previous, &candidate,
                                             s_client.pending_hex,
                                             &s_client.pending_polls);
            publish_snapshot(&candidate);
            failure_backoff_s = 5U;
            xSemaphoreTake(s_client.lock, portMAX_DELAY);
            ++s_client.polls_ok;
            xSemaphoreGive(s_client.lock);
            ESP_LOGI(TAG, "adsb.fi: %lu reports, %lu accepted, nearest=%s",
                     (unsigned long)candidate.aircraft_reported,
                     (unsigned long)candidate.aircraft_accepted,
                     candidate.aircraft_count > 0U
                         ? candidate.aircraft[0].hex : "none");
        } else {
            airtrack_feed_error_t error = AIRTRACK_ERROR_DNS_TLS;
            if (http_status == 429) {
                error = AIRTRACK_ERROR_RATE_LIMIT;
                next_delay_s = retry_after_s > 0U
                                   ? retry_after_s
                                   : (failure_backoff_s < 60U
                                          ? 60U : failure_backoff_s);
                if (next_delay_s > MAX_RATE_BACKOFF_S) {
                    next_delay_s = MAX_RATE_BACKOFF_S;
                }
            } else if (http_status >= 400 && http_status < 500) {
                error = AIRTRACK_ERROR_CONFIG;
                next_delay_s = MAX_RATE_BACKOFF_S;
            } else if (http_status >= 300) {
                error = AIRTRACK_ERROR_HTTP;
                next_delay_s = failure_backoff_s;
            } else if (poll_result == ESP_ERR_INVALID_RESPONSE ||
                       poll_result == ESP_ERR_INVALID_SIZE) {
                error = AIRTRACK_ERROR_PARSE;
                next_delay_s = failure_backoff_s;
            } else {
                next_delay_s = failure_backoff_s;
            }
            publish_failure(error, http_status, next_delay_s);
            xSemaphoreTake(s_client.lock, portMAX_DELAY);
            ++s_client.polls_failed;
            xSemaphoreGive(s_client.lock);
            ESP_LOGW(TAG, "adsb.fi poll failed: %s, HTTP %d; retry %lus",
                     esp_err_to_name(poll_result), http_status,
                     (unsigned long)next_delay_s);
            if (failure_backoff_s < MAX_NORMAL_BACKOFF_S) {
                failure_backoff_s *= 2U;
                if (failure_backoff_s > MAX_NORMAL_BACKOFF_S) {
                    failure_backoff_s = MAX_NORMAL_BACKOFF_S;
                }
            }
        }
        const uint32_t wait_s = retry_after_s > 0U
                                    ? next_delay_s
                                    : jittered_delay_s(next_delay_s);
        if (wait_or_stop(wait_s * 1000U)) {
            break;
        }
    }

    drop_http_client();
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    s_client.running = false;
    s_client.task = NULL;
    xSemaphoreGive(s_client.lock);
    vTaskDelete(NULL);
}

esp_err_t adsb_client_start(const airtrack_settings_t *settings)
{
    if (settings == NULL || airtrack_settings_validate(settings) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client.lock == NULL) {
        s_client.lock = xSemaphoreCreateMutexStatic(&s_client.lock_storage);
    }
    if (s_client.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    if (s_client.running) {
        xSemaphoreGive(s_client.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_client.settings = *settings;
    s_client.stop_requested = false;
    s_client.online = false;
    s_client.pending_hex[0] = '\0';
    s_client.pending_polls = 0U;
    s_client.polls_ok = 0U;
    s_client.polls_failed = 0U;
    s_client.connections = 0U;
    memset(&s_client.snapshot, 0, sizeof(s_client.snapshot));
    s_client.snapshot.state = settings->location_configured
                                  ? AIRTRACK_FEED_OFFLINE
                                  : AIRTRACK_FEED_CONFIG_REQUIRED;
    s_client.snapshot.error = settings->location_configured
                                  ? AIRTRACK_ERROR_WIFI
                                  : AIRTRACK_ERROR_CONFIG;
    s_client.snapshot.config_generation = settings->generation;
    s_client.running = true;
    s_client.task = xTaskCreateStatic(
        worker, "adsb_worker", WORKER_STACK_BYTES, NULL, 3,
        s_client.task_stack, &s_client.task_storage);
    if (s_client.task == NULL) {
        s_client.running = false;
        xSemaphoreGive(s_client.lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_client.lock);
    return ESP_OK;
}

esp_err_t adsb_client_set_online(bool online)
{
    if (s_client.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    if (!s_client.running) {
        xSemaphoreGive(s_client.lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool changed = s_client.online != online;
    s_client.online = online;
    const TaskHandle_t task = s_client.task;
    xSemaphoreGive(s_client.lock);
    if (task != NULL && changed) {
        xTaskNotifyGive(task);
    }
    return ESP_OK;
}

esp_err_t adsb_client_update_settings(const airtrack_settings_t *settings)
{
    if (settings == NULL || airtrack_settings_validate(settings) != ESP_OK ||
        s_client.lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    if (!s_client.running) {
        xSemaphoreGive(s_client.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_client.settings = *settings;
    const TaskHandle_t task = s_client.task;
    xSemaphoreGive(s_client.lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return ESP_OK;
}

esp_err_t adsb_client_get_snapshot(airtrack_snapshot_t *snapshot)
{
    if (snapshot == NULL || s_client.lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    *snapshot = s_client.snapshot;
    xSemaphoreGive(s_client.lock);
    return ESP_OK;
}

esp_err_t adsb_client_get_stats(adsb_client_stats_t *stats)
{
    if (stats == NULL || s_client.lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    stats->polls_ok = s_client.polls_ok;
    stats->polls_failed = s_client.polls_failed;
    stats->connections = s_client.connections;
    xSemaphoreGive(s_client.lock);
    return ESP_OK;
}

esp_err_t adsb_client_stop(void)
{
    if (s_client.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    if (!s_client.running) {
        xSemaphoreGive(s_client.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_client.stop_requested = true;
    const TaskHandle_t task = s_client.task;
    xSemaphoreGive(s_client.lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(20U));
        if (!adsb_client_is_running()) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

bool adsb_client_is_running(void)
{
    if (s_client.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    const bool running = s_client.running;
    xSemaphoreGive(s_client.lock);
    return running;
}
