#include "adsb_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
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
#define ROUTE_CACHE_ENTRIES 12U
#define ROUTE_BODY_MAX_BYTES 2048U
#define ROUTE_HTTP_TIMEOUT_MS 8000
#define ROUTE_RETRY_UNKNOWN_MS (60LL * 60LL * 1000LL)
#define ROUTE_RETRY_FAILED_MS (5LL * 60LL * 1000LL)
/* A followed flight that stops reporting (coverage gap, or transponder off
 * after landing) stays on screen with its growing age for this long. */
#define FOCUS_HOLD_S 1800.0f

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
    adsb_client_hook_t hook;
    void *hook_context;
} adsb_context_t;

/*
 * Identity lookup for a followed flight, owned by the worker task.  The
 * focus text is tried as each plausible identity in turn until one answers;
 * after that the same lookup is kept until the focus changes.
 */
typedef struct {
    char focus[AIRTRACK_FOCUS_MAX_LENGTH + 1U];
    airtrack_focus_kind_t order[AIRTRACK_FOCUS_KINDS_MAX];
    size_t count;
    size_t index;
    bool resolved;
} focus_lookup_t;

static focus_lookup_t s_focus;

static adsb_context_t s_client;

/*
 * Route enrichment cache (adsbdb.com).  One lookup per poll cycle at most,
 * keyed by callsign; unknown callsigns (general aviation) are remembered so
 * they are not re-queried every poll.  Owned by the worker task.
 */
typedef struct {
    char callsign[16];
    bool used;
    bool known;          /* adsbdb returned a route */
    bool failed;         /* transport/parse failure; retry sooner */
    char from[5];
    char to[5];
    bool destination_valid;
    double destination_latitude;
    double destination_longitude;
    bool origin_valid;
    double origin_latitude;
    double origin_longitude;
    char airline_iata[3];
    int64_t fetched_ms;
    /* Direction ADS-B has shown this callsign flying the route: +1 as
     * adsbdb lists it, -1 reversed, 0 not yet seen. */
    int8_t orientation;
    int64_t oriented_ms;
} route_entry_t;

/* A direction seen once holds for the rest of the day's leg; evidence
 * against it (two polls running) flips it sooner. */
#define ROUTE_ORIENTATION_HOLD_MS (12LL * 60LL * 60LL * 1000LL)

static route_entry_t s_routes[ROUTE_CACHE_ENTRIES];

typedef struct {
    char body[ROUTE_BODY_MAX_BYTES + 1U];
    size_t length;
    bool overflow;
} route_response_t;

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
    /* Every poll URL is on the same host, so a new path (a moved location,
     * or the next identity lookup for a followed flight) keeps the session. */
    if (s_http != NULL && esp_http_client_set_url(s_http, url) == ESP_OK) {
        (void)snprintf(s_http_url, sizeof(s_http_url), "%s", url);
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
        .user_agent = "AirTrack/1.5 (ESP32-C6; personal non-commercial)",
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
    int url_length;
    if (settings->focus_flight[0] != '\0' && s_focus.count > 0U) {
        /* The focus text is validated to [A-Z0-9-] with an optional leading
         * '~', all of which are safe in a URL path. */
        url_length = snprintf(url, sizeof(url),
                              "https://opendata.adsb.fi/api/v2/%s/%s",
                              airtrack_focus_kind_path(s_focus.order[s_focus.index]),
                              settings->focus_flight);
    } else {
        url_length = snprintf(
            url, sizeof(url),
            "https://opendata.adsb.fi/api/v3/lat/%.6f/lon/%.6f/dist/%u",
            (double)settings->latitude_e7 / 10000000.0,
            (double)settings->longitude_e7 / 10000000.0,
            (unsigned)settings->radius_nm);
    }
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

static esp_err_t route_http_event(esp_http_client_event_t *event)
{
    route_response_t *response = event != NULL ? event->user_data : NULL;
    if (response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if (response->length + (size_t)event->data_len > ROUTE_BODY_MAX_BYTES) {
            response->overflow = true;
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(response->body + response->length, event->data,
               (size_t)event->data_len);
        response->length += (size_t)event->data_len;
        response->body[response->length] = '\0';
    }
    return ESP_OK;
}

static bool copy_airport_code(const cJSON *airport, char out[5])
{
    out[0] = '\0';
    if (!cJSON_IsObject(airport)) {
        return false;
    }
    const cJSON *iata = cJSON_GetObjectItemCaseSensitive(airport, "iata_code");
    const cJSON *icao = cJSON_GetObjectItemCaseSensitive(airport, "icao_code");
    const cJSON *pick = cJSON_IsString(iata) && iata->valuestring != NULL &&
                                strlen(iata->valuestring) == 3U
                            ? iata
                            : cJSON_IsString(icao) && icao->valuestring != NULL
                                  ? icao : NULL;
    if (pick == NULL) {
        return false;
    }
    size_t used = 0U;
    for (const char *cursor = pick->valuestring; *cursor != '\0' && used < 4U;
         ++cursor) {
        const unsigned char byte = (unsigned char)*cursor;
        if ((byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')) {
            out[used++] = (char)byte;
        } else if (byte >= 'a' && byte <= 'z') {
            out[used++] = (char)(byte - 'a' + 'A');
        }
    }
    out[used] = '\0';
    return used >= 3U;
}

static bool airport_position(const cJSON *airport, double *latitude,
                             double *longitude)
{
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(airport, "latitude");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(airport, "longitude");
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon) ||
        lat->valuedouble < -90.0 || lat->valuedouble > 90.0 ||
        lon->valuedouble < -180.0 || lon->valuedouble > 180.0) {
        return false;
    }
    *latitude = lat->valuedouble;
    *longitude = lon->valuedouble;
    return true;
}

static bool callsign_url_safe(const char *callsign)
{
    const size_t length = strnlen(callsign, 9U);
    if (length < 3U || length > 8U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char byte = (unsigned char)callsign[index];
        if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9'))) {
            return false;
        }
    }
    return true;
}

/* Query adsbdb.com once for one callsign and fill the cache entry. */
static void route_lookup(route_entry_t *entry)
{
    entry->fetched_ms = monotonic_ms();
    entry->known = false;
    entry->failed = true;
    entry->from[0] = '\0';
    entry->to[0] = '\0';
    entry->destination_valid = false;
    entry->origin_valid = false;
    entry->airline_iata[0] = '\0';
    if (!callsign_url_safe(entry->callsign)) {
        entry->failed = false; /* never valid; treat as unknown */
        return;
    }
    char url[96];
    const int url_length = snprintf(url, sizeof(url),
                                    "https://api.adsbdb.com/v0/callsign/%s",
                                    entry->callsign);
    if (url_length < 0 || (size_t)url_length >= sizeof(url)) {
        return;
    }
    route_response_t *response = calloc(1U, sizeof(*response));
    if (response == NULL) {
        return;
    }
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = route_http_event,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = ROUTE_HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_agent = "AirTrack/1.5 (ESP32-C6; personal non-commercial)",
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(response);
        return;
    }
    (void)esp_http_client_set_header(client, "Accept", "application/json");
    const esp_err_t status = esp_http_client_perform(client);
    const int http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status == ESP_OK && !response->overflow &&
        (http_status == 200 || http_status == 404)) {
        entry->failed = false;
        cJSON *root = cJSON_ParseWithLength(response->body, response->length);
        const cJSON *body = cJSON_GetObjectItemCaseSensitive(root, "response");
        const cJSON *route = cJSON_GetObjectItemCaseSensitive(body, "flightroute");
        if (cJSON_IsObject(route)) {
            const cJSON *origin = cJSON_GetObjectItemCaseSensitive(route, "origin");
            const cJSON *destination =
                cJSON_GetObjectItemCaseSensitive(route, "destination");
            char from[5];
            char to[5];
            if (copy_airport_code(origin, from) && copy_airport_code(destination, to)) {
                memcpy(entry->from, from, sizeof(from));
                memcpy(entry->to, to, sizeof(to));
                entry->known = true;
                entry->destination_valid = airport_position(
                    destination, &entry->destination_latitude,
                    &entry->destination_longitude);
                entry->origin_valid = airport_position(
                    origin, &entry->origin_latitude, &entry->origin_longitude);
                const cJSON *airline =
                    cJSON_GetObjectItemCaseSensitive(route, "airline");
                const cJSON *iata = cJSON_GetObjectItemCaseSensitive(airline, "iata");
                if (cJSON_IsString(iata) && iata->valuestring != NULL &&
                    strlen(iata->valuestring) == 2U &&
                    isalnum((unsigned char)iata->valuestring[0]) &&
                    isalnum((unsigned char)iata->valuestring[1])) {
                    entry->airline_iata[0] =
                        (char)toupper((unsigned char)iata->valuestring[0]);
                    entry->airline_iata[1] =
                        (char)toupper((unsigned char)iata->valuestring[1]);
                    entry->airline_iata[2] = '\0';
                }
            }
        }
        cJSON_Delete(root);
        ESP_LOGI(TAG, "route %s: %s", entry->callsign,
                 entry->known ? entry->to : "unknown");
    } else {
        ESP_LOGW(TAG, "route lookup for %s failed: %s (HTTP %d)",
                 entry->callsign, esp_err_to_name(status), http_status);
    }
    free(response);
}

static route_entry_t *route_cache_find(const char *callsign)
{
    for (size_t index = 0U; index < ROUTE_CACHE_ENTRIES; ++index) {
        if (s_routes[index].used &&
            strcmp(s_routes[index].callsign, callsign) == 0) {
            return &s_routes[index];
        }
    }
    return NULL;
}

static route_entry_t *route_cache_allocate(const char *callsign)
{
    route_entry_t *victim = &s_routes[0];
    for (size_t index = 0U; index < ROUTE_CACHE_ENTRIES; ++index) {
        if (!s_routes[index].used) {
            victim = &s_routes[index];
            break;
        }
        if (s_routes[index].fetched_ms < victim->fetched_ms) {
            victim = &s_routes[index];
        }
    }
    memset(victim, 0, sizeof(*victim));
    (void)snprintf(victim->callsign, sizeof(victim->callsign), "%s", callsign);
    victim->used = true;
    return victim;
}

static void apply_route(airtrack_aircraft_t *aircraft, const route_entry_t *entry)
{
    aircraft->route_valid = entry->known;
    if (entry->known) {
        memcpy(aircraft->route_from, entry->from, sizeof(aircraft->route_from));
        memcpy(aircraft->route_to, entry->to, sizeof(aircraft->route_to));
        aircraft->destination_valid = entry->destination_valid;
        aircraft->destination_latitude = entry->destination_latitude;
        aircraft->destination_longitude = entry->destination_longitude;
        aircraft->origin_valid = entry->origin_valid;
        aircraft->origin_latitude = entry->origin_latitude;
        aircraft->origin_longitude = entry->origin_longitude;
        memcpy(aircraft->airline_iata, entry->airline_iata,
               sizeof(aircraft->airline_iata));
    }
}

/*
 * adsbdb gives a callsign's usual route, and out-and-back flight numbers
 * share one entry, so its order may be backwards for today's leg.  Settle
 * the direction from what ADS-B shows and put the aircraft's route in that
 * order; without evidence yet (at the gate) the database order stands,
 * unconfirmed.
 */
static void orient_route(airtrack_aircraft_t *aircraft, route_entry_t *entry, int64_t now)
{
    if (!entry->known) {
        return;
    }
    const int evidence = airtrack_route_evidence(aircraft);
    if (evidence != 0) {
        if (entry->orientation == 0 || evidence == entry->orientation) {
            entry->orientation = (int8_t)evidence;
            entry->oriented_ms = now;
        } else if (now - entry->oriented_ms > 30LL * 1000LL) {
            /* Contrary evidence that persists: a new leg the other way. */
            entry->orientation = (int8_t)evidence;
            entry->oriented_ms = now;
        }
    }
    if (entry->orientation != 0 && now - entry->oriented_ms > ROUTE_ORIENTATION_HOLD_MS) {
        entry->orientation = 0;
    }
    if (entry->orientation < 0) {
        airtrack_route_reverse(aircraft);
    }
    aircraft->route_confirmed = entry->orientation != 0;
}

/* Reset the identity rotation when the followed flight changes. */
static void focus_sync(const airtrack_settings_t *settings)
{
    if (strcmp(s_focus.focus, settings->focus_flight) == 0) {
        return;
    }
    memset(&s_focus, 0, sizeof(s_focus));
    memcpy(s_focus.focus, settings->focus_flight, sizeof(s_focus.focus));
    s_focus.count = airtrack_focus_lookup_order(settings->focus_flight,
                                                s_focus.order);
}

/*
 * Settle a followed flight's poll.  A hit pins the lookup kind.  A miss
 * either keeps showing the last position (with its true, growing age) while
 * it is recent, or moves on to the next plausible identity lookup.
 */
static void focus_after_poll(const airtrack_snapshot_t *previous,
                             airtrack_snapshot_t *candidate)
{
    if (candidate->aircraft_count > 0U) {
        s_focus.resolved = true;
        return;
    }
    if (previous->aircraft_count > 0U &&
        airtrack_aircraft_matches(&previous->aircraft[0], s_focus.focus) &&
        previous->last_success_monotonic_ms > 0) {
        const float elapsed_s =
            (float)(monotonic_ms() - previous->last_success_monotonic_ms) / 1000.0f;
        const float age = previous->aircraft[0].seen_pos_s +
                          (elapsed_s > 0.0f ? elapsed_s : 0.0f);
        if (age <= FOCUS_HOLD_S) {
            candidate->aircraft[0] = previous->aircraft[0];
            candidate->aircraft[0].seen_pos_s = age;
            candidate->aircraft_count = 1U;
            candidate->state = AIRTRACK_FEED_LIVE;
            return;
        }
    }
    if (!s_focus.resolved && s_focus.count > 1U) {
        s_focus.index = (s_focus.index + 1U) % s_focus.count;
    }
}

/*
 * Fill route fields from the cache; returns the first aircraft whose
 * callsign still needs a network lookup (or NULL).
 */
static airtrack_aircraft_t *enrich_from_cache(airtrack_snapshot_t *snapshot)
{
    airtrack_aircraft_t *pending = NULL;
    const int64_t now = monotonic_ms();
    for (size_t index = 0U; index < snapshot->aircraft_count; ++index) {
        airtrack_aircraft_t *aircraft = &snapshot->aircraft[index];
        if (aircraft->callsign[0] == '\0') {
            continue;
        }
        route_entry_t *entry = route_cache_find(aircraft->callsign);
        if (entry != NULL) {
            const int64_t age = now - entry->fetched_ms;
            const bool expired = entry->failed ? age > ROUTE_RETRY_FAILED_MS
                                 : !entry->known ? age > ROUTE_RETRY_UNKNOWN_MS
                                                 : false;
            if (!expired) {
                apply_route(aircraft, entry);
                orient_route(aircraft, entry, now);
                continue;
            }
        }
        if (pending == NULL) {
            pending = aircraft;
        }
    }
    return pending;
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
        focus_sync(&settings);

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
            if (settings.focus_flight[0] != '\0') {
                focus_after_poll(&previous, &candidate);
            }
            airtrack_apply_target_hysteresis(&previous, &candidate,
                                             s_client.pending_hex,
                                             &s_client.pending_polls);
            airtrack_aircraft_t *pending = enrich_from_cache(&candidate);
            publish_snapshot(&candidate);
            if (pending != NULL) {
                /* One bounded lookup per cycle, after the poll is visible. */
                route_entry_t *entry = route_cache_find(pending->callsign);
                if (entry == NULL) {
                    entry = route_cache_allocate(pending->callsign);
                }
                route_lookup(entry);
                (void)enrich_from_cache(&candidate);
                candidate.sequence++;
                publish_snapshot(&candidate);
            }
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
        /* Further enrichment runs here, after the poll is published, so its
         * requests never overlap this worker's own TLS traffic. */
        xSemaphoreTake(s_client.lock, portMAX_DELAY);
        const adsb_client_hook_t hook = s_client.hook;
        void *const hook_context = s_client.hook_context;
        xSemaphoreGive(s_client.lock);
        if (hook != NULL) {
            /* Static rather than on this stack, which TLS handshakes in the
             * hook need; only this task touches it. */
            static airtrack_snapshot_t published;
            published = current_snapshot();
            hook(&settings, &published, hook_context);
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

esp_err_t adsb_client_set_hook(adsb_client_hook_t hook, void *context)
{
    if (s_client.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    s_client.hook = hook;
    s_client.hook_context = context;
    xSemaphoreGive(s_client.lock);
    return ESP_OK;
}

void adsb_client_wake(void)
{
    if (s_client.lock == NULL) {
        return;
    }
    xSemaphoreTake(s_client.lock, portMAX_DELAY);
    const TaskHandle_t task = s_client.running ? s_client.task : NULL;
    xSemaphoreGive(s_client.lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
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
