#include "flight_info.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "flight_png.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "flight_info";

#define HTTP_TIMEOUT_MS 8000
#define USER_AGENT "AirTrack/1.9 (ESP32-C6; personal non-commercial)"
#define ROUTE_BODY_MAX 3072U
#define SCHEDULE_BODY_MAX 4096U
#define USAGE_BODY_MAX 1536U
#define LOGO_BODY_MAX 8192U
/* LCD background (UI_COLOR_BG) that logo transparency is blended onto. */
#define LOGO_BACKGROUND 0x07111FUL

/*
 * Flystack budget.  The free plan is 100 requests a month plus a one-off
 * credit pool, so a followed flight costs at most: one lookup when it is
 * chosen (none if the dashboard check already fetched it), one at takeoff
 * and one on approach for gates, each only if the last is 20 minutes old.
 * A rolling daily cap (FLIGHT_INFO_DAILY_CAP) and Flystack's own remaining
 * count stop runaway use.
 */
#define FLYSTACK_MIN_REMAINING 3
#define SCHEDULE_EVENT_REFRESH_MS (20LL * 60LL * 1000LL)
#define SCHEDULE_STALE_MS (6LL * 3600LL * 1000LL)
#define SCHEDULE_RETRY_ERROR_MS (10LL * 60LL * 1000LL)
#define SCHEDULE_RETRY_NOT_FOUND_MS (3LL * 3600LL * 1000LL)
#define SCHEDULE_RETRY_DENIED_MS (6LL * 3600LL * 1000LL)
#define BOOT_GRACE_MS 20000LL
#define DAY_MS (24LL * 3600LL * 1000LL)
#define ROUTE_RETRY_MS (5LL * 60LL * 1000LL)
#define LOGO_RETRY_MS (30LL * 60LL * 1000LL)
#define USAGE_REFRESH_MS (6LL * 3600LL * 1000LL)
#define TRAIL_STEP_NM 3.0f

typedef struct {
    char *body;
    size_t length;
    size_t capacity;
    bool overflow;
} body_t;

typedef struct {
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    void (*wake)(void);

    /* Shared with readers; guarded by lock. */
    flight_info_t info;
    flight_trail_point_t trail[FLIGHT_INFO_TRAIL_MAX];
    uint16_t logo[FLIGHT_INFO_LOGO_SIZE * FLIGHT_INFO_LOGO_SIZE];
    char logo_pixels_iata[3];
    uint8_t *logo_png;
    size_t logo_png_length;
    flight_check_t check;
    int64_t check_done_ms;
    flight_quota_t quota;
    char key[AIRTRACK_FLYSTACK_KEY_MAX_LENGTH + 1U];
    bool usage_dirty;
    bool schedule_events;       /* takeoff/approach seen since last lookup */

    /* Worker only. */
    char route_key[sizeof(((airtrack_aircraft_t *)0)->callsign)];
    bool route_done;
    int64_t route_attempt_ms;
    int64_t schedule_attempt_ms;
    int64_t schedule_fetch_ms;
    bool approach_seen;
    char logo_failed_iata[3];
    int64_t logo_attempt_ms;
    int64_t usage_fetched_ms;
    int64_t day_start_ms;
} flight_state_t;

static flight_state_t s_flight;

static int64_t monotonic_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static void lock(void)
{
    xSemaphoreTake(s_flight.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_flight.lock);
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    body_t *body = event != NULL ? event->user_data : NULL;
    if (body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if (body->length + (size_t)event->data_len >= body->capacity) {
            body->overflow = true;
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(body->body + body->length, event->data, (size_t)event->data_len);
        body->length += (size_t)event->data_len;
        body->body[body->length] = '\0';
    }
    return ESP_OK;
}

/* One bounded HTTPS GET into a fresh heap buffer; the caller frees body. */
static esp_err_t https_get(const char *url, const char *api_key, size_t capacity,
                           body_t *body, int *status)
{
    *status = 0;
    *body = (body_t) {.capacity = capacity};
    body->body = malloc(capacity);
    if (body->body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    body->body[0] = '\0';
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = body,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 768,
        .user_agent = USER_AGENT,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(body->body);
        body->body = NULL;
        return ESP_ERR_NO_MEM;
    }
    (void)esp_http_client_set_header(client, "Accept", "application/json");
    if (api_key != NULL) {
        (void)esp_http_client_set_header(client, "x-api-key", api_key);
    }
    esp_err_t result = esp_http_client_perform(client);
    *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result == ESP_OK && body->overflow) {
        result = ESP_ERR_INVALID_SIZE;
    }
    return result;
}

static bool code_url_safe(const char *code)
{
    const size_t length = strnlen(code, FLIGHT_INFO_CODE_MAX);
    if (length < 2U || length >= FLIGHT_INFO_CODE_MAX) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char byte = code[index];
        if (!((byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9'))) {
            return false;
        }
    }
    return true;
}

/* "AS555": a two-character IATA airline designator and a flight number. */
static bool iata_flight_number(const char *code)
{
    const size_t length = strnlen(code, FLIGHT_INFO_CODE_MAX);
    if (length < 3U || length > 7U || !code_url_safe(code)) {
        return false;
    }
    const bool designator =
        ((code[0] >= 'A' && code[0] <= 'Z') || (code[1] >= 'A' && code[1] <= 'Z'));
    return designator && code[2] >= '0' && code[2] <= '9' &&
           !airtrack_is_airline_callsign(code);
}

/* adsbdb: returns true when the service answered (known or unknown). */
static bool route_lookup(const char *callsign, flight_route_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!code_url_safe(callsign)) {
        return true;
    }
    char url[80];
    (void)snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", callsign);
    body_t body;
    int status = 0;
    const esp_err_t result = https_get(url, NULL, ROUTE_BODY_MAX, &body, &status);
    bool answered = false;
    if (result == ESP_OK && (status == 200 || status == 404)) {
        answered = true;
        (void)flight_info_parse_route(body.body, body.length, out);
    }
    ESP_LOGI(TAG, "adsbdb %s: %s", callsign,
             !answered ? "failed" : out->valid ? out->callsign_icao : "unknown");
    free(body.body);
    return answered;
}

static void roll_day(int64_t now)
{
    if (s_flight.day_start_ms == 0 || now - s_flight.day_start_ms >= DAY_MS) {
        s_flight.day_start_ms = now;
        lock();
        s_flight.quota.calls_today = 0U;
        unlock();
    }
}

static bool budget_available(void)
{
    lock();
    const bool ok = s_flight.quota.calls_today < FLIGHT_INFO_DAILY_CAP &&
                    (!s_flight.quota.usage_valid ||
                     s_flight.quota.remaining > FLYSTACK_MIN_REMAINING);
    unlock();
    return ok;
}

/* Flystack flight lookup by ICAO ("flight_icao") or IATA ("flight_iata"). */
static flight_schedule_state_t schedule_lookup(const char *key, const char *parameter,
                                               const char *code, flight_schedule_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!code_url_safe(code)) {
        return FLIGHT_SCHEDULE_UNSUPPORTED;
    }
    char url[96];
    (void)snprintf(url, sizeof(url),
                   "https://api.flystack.dev/v1/flights/lookup?%s=%s", parameter, code);
    body_t body;
    int status = 0;
    const esp_err_t result = https_get(url, key, SCHEDULE_BODY_MAX, &body, &status);
    flight_schedule_state_t state;
    if (result != ESP_OK && status == 0) {
        state = FLIGHT_SCHEDULE_ERROR;
    } else if (status == 200 || status == 201) {
        /* Billed, whatever the body says. */
        lock();
        ++s_flight.quota.calls_today;
        s_flight.usage_dirty = true;
        unlock();
        state = result == ESP_OK
                    ? flight_info_parse_schedule(body.body, body.length, out)
                    : FLIGHT_SCHEDULE_ERROR;
    } else if (status == 404) {
        state = FLIGHT_SCHEDULE_NOT_FOUND;
    } else if (status == 401 || status == 403) {
        state = FLIGHT_SCHEDULE_UNAUTHORIZED;
    } else if (status == 402 || status == 429) {
        state = FLIGHT_SCHEDULE_QUOTA;
    } else {
        state = FLIGHT_SCHEDULE_ERROR;
    }
    if (state == FLIGHT_SCHEDULE_OK) {
        out->fetched_epoch = (int64_t)time(NULL);
    }
    ESP_LOGI(TAG, "Flystack %s=%s: HTTP %d, %s", parameter, code, status,
             flight_schedule_state_name(state));
    lock();
    s_flight.quota.last_error = state == FLIGHT_SCHEDULE_OK ||
                                        state == FLIGHT_SCHEDULE_NOT_FOUND
                                    ? FLIGHT_SCHEDULE_OK : state;
    unlock();
    free(body.body);
    return state;
}

static void usage_lookup(const char *key)
{
    body_t body;
    int status = 0;
    const esp_err_t result = https_get("https://api.flystack.dev/v1/usage", key,
                                       USAGE_BODY_MAX, &body, &status);
    int32_t remaining = 0;
    char renewal[12] = "";
    const bool ok = result == ESP_OK && status == 200 &&
                    flight_info_parse_usage(body.body, body.length, &remaining, renewal);
    free(body.body);
    lock();
    s_flight.usage_dirty = false;
    if (ok) {
        s_flight.quota.usage_valid = true;
        s_flight.quota.remaining = remaining;
        memcpy(s_flight.quota.renewal, renewal, sizeof(renewal));
    } else if (status == 401 || status == 403) {
        s_flight.quota.usage_valid = false;
        s_flight.quota.last_error = FLIGHT_SCHEDULE_UNAUTHORIZED;
    }
    unlock();
    ESP_LOGI(TAG, "Flystack usage: HTTP %d, %ld remaining", status,
             ok ? (long)remaining : -1L);
}

static bool logo_fetch(const char *iata)
{
    char url[64];
    (void)snprintf(url, sizeof(url), "https://pics.avs.io/al_square/%u/%u/%s.png",
                   (unsigned)FLIGHT_INFO_LOGO_WEB_SIZE, (unsigned)FLIGHT_INFO_LOGO_WEB_SIZE,
                   iata);
    body_t body;
    int status = 0;
    esp_err_t result = https_get(url, NULL, LOGO_BODY_MAX, &body, &status);
    uint16_t *pixels = NULL;
    uint8_t *png = NULL;
    if (result == ESP_OK && status == 200) {
        pixels = malloc(sizeof(s_flight.logo));
        png = malloc(body.length);
        result = pixels == NULL || png == NULL ? ESP_ERR_NO_MEM
            : flight_png_decode_rgb565((const uint8_t *)body.body, body.length, pixels,
                                       FLIGHT_INFO_LOGO_SIZE, FLIGHT_INFO_LOGO_SIZE,
                                       LOGO_BACKGROUND);
        if (result == ESP_OK) {
            memcpy(png, body.body, body.length);
        }
    } else if (result == ESP_OK) {
        result = ESP_ERR_NOT_FOUND;
    }
    const size_t png_length = body.length;
    free(body.body);
    ESP_LOGI(TAG, "logo %s: HTTP %d, %s", iata, status, esp_err_to_name(result));
    if (result == ESP_OK) {
        lock();
        /* Kept for the dashboard, which loads it from the device so that a
         * browser blocking the logo host still shows it. */
        free(s_flight.logo_png);
        s_flight.logo_png = png;
        s_flight.logo_png_length = png_length;
        png = NULL;
        memcpy(s_flight.logo, pixels, sizeof(s_flight.logo));
        memcpy(s_flight.logo_pixels_iata, iata, sizeof(s_flight.logo_pixels_iata));
        s_flight.info.logo_valid = true;
        memcpy(s_flight.info.logo_iata, iata, sizeof(s_flight.info.logo_iata));
        ++s_flight.info.logo_generation;
        ++s_flight.info.generation;
        unlock();
    }
    free(pixels);
    free(png);
    return result == ESP_OK;
}

/* Callers hold the lock. */
static void reset_focus_locked(const char *focus, int64_t now)
{
    flight_info_t *info = &s_flight.info;
    const uint32_t logo_generation = info->logo_generation;
    const uint32_t generation = info->generation;
    memset(info, 0, sizeof(*info));
    info->logo_generation = logo_generation;
    info->generation = generation + 1U;
    (void)snprintf(info->code, sizeof(info->code), "%s", focus);
    info->schedule_state = s_flight.quota.key_set ? FLIGHT_SCHEDULE_IDLE
                                                  : FLIGHT_SCHEDULE_NO_KEY;
    s_flight.schedule_events = false;
    s_flight.route_key[0] = '\0';
    s_flight.route_done = false;
    s_flight.route_attempt_ms = 0;
    s_flight.schedule_attempt_ms = 0;
    s_flight.schedule_fetch_ms = 0;
    s_flight.approach_seen = false;

    /* A dashboard check of this flight already paid for its details. */
    const flight_check_t *check = &s_flight.check;
    if (focus[0] != '\0' && check->state == FLIGHT_CHECK_DONE &&
        now - s_flight.check_done_ms < SCHEDULE_EVENT_REFRESH_MS &&
        (strcmp(check->query, focus) == 0 ||
         strcmp(check->route.callsign_icao, focus) == 0)) {
        if (check->route.valid) {
            info->route = check->route;
            s_flight.route_done = true;
            (void)snprintf(s_flight.route_key, sizeof(s_flight.route_key), "%s", focus);
        }
        if (check->schedule_requested && check->schedule_state == FLIGHT_SCHEDULE_OK) {
            info->schedule = check->schedule;
            info->schedule_state = FLIGHT_SCHEDULE_OK;
            s_flight.schedule_fetch_ms = s_flight.check_done_ms;
            s_flight.schedule_attempt_ms = s_flight.check_done_ms;
        }
    }
}

static void trail_append_locked(const airtrack_aircraft_t *aircraft)
{
    flight_info_t *info = &s_flight.info;
    const int16_t altitude = aircraft->ground ? -1
        : aircraft->altitude_valid ? (int16_t)(aircraft->altitude_ft / 100) : 0;
    if (info->trail_count > 0U) {
        const flight_trail_point_t *last = &s_flight.trail[info->trail_count - 1U];
        float distance = 0.0f;
        float bearing = 0.0f;
        airtrack_geometry(last->latitude, last->longitude, aircraft->latitude,
                          aircraft->longitude, &distance, &bearing);
        const bool ground_changed = (last->altitude_hft < 0) != aircraft->ground;
        if (distance < TRAIL_STEP_NM && !ground_changed) {
            return;
        }
    }
    if (info->trail_count >= FLIGHT_INFO_TRAIL_MAX) {
        /* Halve the resolution to keep takeoff-to-landing in view. */
        uint16_t kept = 0U;
        for (uint16_t index = 0U; index < info->trail_count; index += 2U) {
            s_flight.trail[kept++] = s_flight.trail[index];
        }
        info->trail_count = kept;
    }
    s_flight.trail[info->trail_count++] = (flight_trail_point_t) {
        .latitude = (float)aircraft->latitude,
        .longitude = (float)aircraft->longitude,
        .altitude_hft = altitude,
    };
    ++info->generation;
}

static void update_live_locked(const airtrack_snapshot_t *snapshot)
{
    flight_info_t *info = &s_flight.info;
    const airtrack_aircraft_t *aircraft =
        snapshot->aircraft_count > 0U &&
                airtrack_aircraft_matches(&snapshot->aircraft[0], info->code)
            ? &snapshot->aircraft[0] : NULL;
    const int64_t now = (int64_t)time(NULL);
    airtrack_flight_phase_t phase;
    int64_t eta = 0;
    if (aircraft == NULL) {
        /* Out of sight: before departure, or after the hold expired. */
        phase = info->phase == AIRTRACK_PHASE_LANDED ? AIRTRACK_PHASE_LANDED
            : !info->was_airborne ? AIRTRACK_PHASE_UNKNOWN
            : info->phase == AIRTRACK_PHASE_APPROACH ? AIRTRACK_PHASE_LANDED
                                                     : AIRTRACK_PHASE_LOST;
    } else {
        const int64_t position_epoch = now - (int64_t)aircraft->seen_pos_s;
        const bool airborne = !aircraft->ground &&
            ((aircraft->altitude_valid && aircraft->altitude_ft > 1000) ||
             (aircraft->ground_speed_valid && aircraft->ground_speed_kt > 100.0f));
        if (airborne && info->phase == AIRTRACK_PHASE_LANDED) {
            /* Same identity, next leg: start a fresh track. */
            info->trail_count = 0U;
            info->was_airborne = false;
            info->takeoff_epoch = 0;
            info->landing_epoch = 0;
            s_flight.route_done = false;
            s_flight.approach_seen = false;
        }
        if (airborne && !info->was_airborne) {
            info->was_airborne = true;
            s_flight.schedule_events = true; /* takeoff */
            /* Only a takeoff actually watched gets a time: seen on the
             * ground before, or first seen still low in the climb. */
            const bool watched = info->phase == AIRTRACK_PHASE_GROUND ||
                (aircraft->altitude_valid && aircraft->altitude_ft < 5000 &&
                 aircraft->vertical_rate_valid && aircraft->vertical_rate_fpm > 0);
            info->takeoff_epoch = watched ? position_epoch : 0;
        }
        /* Put adsbdb's route (with its city names) in the direction ADS-B
         * shows the aircraft flying. */
        flight_route_t *route = &info->route;
        if (aircraft->route_confirmed && route->valid &&
            strcmp(route->origin, aircraft->route_to) == 0 &&
            strcmp(route->destination, aircraft->route_from) == 0) {
            char code[sizeof(route->origin)];
            char city[sizeof(route->origin_city)];
            memcpy(code, route->origin, sizeof(code));
            memcpy(route->origin, route->destination, sizeof(code));
            memcpy(route->destination, code, sizeof(code));
            memcpy(city, route->origin_city, sizeof(city));
            memcpy(route->origin_city, route->destination_city, sizeof(city));
            memcpy(route->destination_city, city, sizeof(city));
            ++info->generation;
        }
        if (info->route_confirmed != aircraft->route_confirmed) {
            info->route_confirmed = aircraft->route_confirmed;
            ++info->generation;
        }
        /* Distance to go only means something once the direction is known. */
        float remaining = -1.0f;
        if (aircraft->destination_valid && aircraft->route_confirmed) {
            float bearing = 0.0f;
            airtrack_geometry(aircraft->latitude, aircraft->longitude,
                              aircraft->destination_latitude,
                              aircraft->destination_longitude, &remaining, &bearing);
        }
        phase = airtrack_flight_phase(aircraft, aircraft->seen_pos_s,
                                      info->was_airborne, remaining);
        if (phase == AIRTRACK_PHASE_APPROACH && !s_flight.approach_seen) {
            s_flight.approach_seen = true;
            s_flight.schedule_events = true;
        }
        /* Touchdown watched from the air; a flight first seen already on the
         * ground at its destination has no landing time. */
        if (phase == AIRTRACK_PHASE_LANDED && info->phase != AIRTRACK_PHASE_LANDED &&
            info->was_airborne) {
            info->landing_epoch = position_epoch;
        }
        if (info->was_airborne && phase != AIRTRACK_PHASE_LANDED && remaining >= 0.0f &&
            aircraft->ground_speed_valid && aircraft->ground_speed_kt >= 60.0f) {
            /* Still climbing, an airliner is well short of its cruise speed;
             * dividing by today's ground speed would overstate the flight
             * by the better part of an hour on a long leg. */
            const bool airliner = aircraft->category[0] == 'A' &&
                                  aircraft->category[1] >= '3' && aircraft->category[1] <= '5';
            float speed = aircraft->ground_speed_kt;
            if (phase == AIRTRACK_PHASE_CLIMB && airliner && remaining > 150.0f &&
                speed < 440.0f) {
                speed = 440.0f;
            }
            eta = position_epoch + (int64_t)(remaining / speed * 3600.0f);
        }
        trail_append_locked(aircraft);
    }
    /* The estimate moves every poll; only a minute's change is news. */
    if (eta == 0 || info->eta_epoch == 0 || llabs(eta - info->eta_epoch) >= 60) {
        info->eta_epoch = eta;
    }
    if (phase != info->phase) {
        info->phase = phase;
        ++info->generation;
    }
}

static bool schedule_due(int64_t now)
{
    const flight_info_t *info = &s_flight.info;
    const int64_t since_attempt = now - s_flight.schedule_attempt_ms;
    switch (info->schedule_state) {
    case FLIGHT_SCHEDULE_IDLE:
        return now > BOOT_GRACE_MS;
    case FLIGHT_SCHEDULE_OK:
        return (s_flight.schedule_events &&
                now - s_flight.schedule_fetch_ms > SCHEDULE_EVENT_REFRESH_MS) ||
               now - s_flight.schedule_fetch_ms > SCHEDULE_STALE_MS;
    case FLIGHT_SCHEDULE_NOT_FOUND:
        return since_attempt > SCHEDULE_RETRY_NOT_FOUND_MS;
    case FLIGHT_SCHEDULE_UNAUTHORIZED:
    case FLIGHT_SCHEDULE_QUOTA:
        return since_attempt > SCHEDULE_RETRY_DENIED_MS;
    case FLIGHT_SCHEDULE_ERROR:
        return since_attempt > SCHEDULE_RETRY_ERROR_MS;
    default:
        return false;
    }
}

static void run_check(void)
{
    static flight_check_t work;
    char key[AIRTRACK_FLYSTACK_KEY_MAX_LENGTH + 1U];
    lock();
    work = s_flight.check;
    memcpy(key, s_flight.key, sizeof(key));
    const bool key_set = s_flight.quota.key_set;
    unlock();

    work.route_answered = route_lookup(work.query, &work.route);
    work.schedule_state = FLIGHT_SCHEDULE_IDLE;
    if (work.schedule_requested) {
        const bool icao = work.route.valid && work.route.callsign_icao[0] != '\0';
        const char *code = icao ? work.route.callsign_icao : work.query;
        const bool iata = !icao && iata_flight_number(code);
        if (!key_set) {
            work.schedule_state = FLIGHT_SCHEDULE_NO_KEY;
        } else if (!icao && !iata && !airtrack_is_airline_callsign(code)) {
            work.schedule_state = FLIGHT_SCHEDULE_UNSUPPORTED;
        } else if (!budget_available()) {
            work.schedule_state = FLIGHT_SCHEDULE_QUOTA;
        } else {
            work.schedule_state = schedule_lookup(key, iata ? "flight_iata" : "flight_icao",
                                                  code, &work.schedule);
        }
    }
    memset(key, 0, sizeof(key));
    lock();
    /* A newer request that arrived meanwhile wins. */
    if (s_flight.check.state == FLIGHT_CHECK_PENDING &&
        strcmp(s_flight.check.query, work.query) == 0 &&
        s_flight.check.schedule_requested == work.schedule_requested) {
        work.state = FLIGHT_CHECK_DONE;
        s_flight.check = work;
        s_flight.check_done_ms = monotonic_ms();
    }
    unlock();
}

void flight_info_service(const airtrack_settings_t *settings,
                         const airtrack_snapshot_t *snapshot, void *context)
{
    (void)context;
    if (s_flight.lock == NULL || settings == NULL || snapshot == NULL) {
        return;
    }
    const int64_t now = monotonic_ms();
    roll_day(now);

    lock();
    if (strcmp(s_flight.info.code, settings->focus_flight) != 0) {
        reset_focus_locked(settings->focus_flight, now);
    }
    if (s_flight.info.code[0] != '\0') {
        update_live_locked(snapshot);
    }
    const bool check_pending = s_flight.check.state == FLIGHT_CHECK_PENDING;
    const bool key_set = s_flight.quota.key_set;
    const bool usage_due = key_set &&
        (s_flight.usage_dirty || s_flight.usage_fetched_ms == 0 ||
         now - s_flight.usage_fetched_ms > USAGE_REFRESH_MS);
    static flight_info_t info;
    info = s_flight.info;
    unlock();

    /* One job per cycle, most wanted first. */
    if (check_pending) {
        run_check();
        return;
    }

    const bool focused = info.code[0] != '\0';
    const airtrack_aircraft_t *aircraft =
        focused && snapshot->aircraft_count > 0U &&
                airtrack_aircraft_matches(&snapshot->aircraft[0], info.code)
            ? &snapshot->aircraft[0] : NULL;

    /* Route and airline for the callsign actually broadcast, falling back
     * to the focus text when it is itself an airline callsign. */
    const char *callsign = aircraft != NULL && aircraft->callsign[0] != '\0'
                               ? aircraft->callsign
                           : airtrack_is_airline_callsign(info.code) ? info.code : "";
    if (focused && callsign[0] != '\0' &&
        (strcmp(callsign, s_flight.route_key) != 0 ||
         (!s_flight.route_done && now - s_flight.route_attempt_ms > ROUTE_RETRY_MS))) {
        (void)snprintf(s_flight.route_key, sizeof(s_flight.route_key), "%s", callsign);
        s_flight.route_attempt_ms = now;
        static flight_route_t route;
        s_flight.route_done = route_lookup(callsign, &route);
        lock();
        if (strcmp(s_flight.info.code, info.code) == 0) {
            s_flight.info.route = route;
            /* A hex or registration now flying an airline callsign can be
             * looked up after all. */
            if (s_flight.info.schedule_state == FLIGHT_SCHEDULE_UNSUPPORTED &&
                airtrack_is_airline_callsign(callsign)) {
                s_flight.info.schedule_state = FLIGHT_SCHEDULE_IDLE;
            }
            ++s_flight.info.generation;
        }
        unlock();
        return;
    }

    if (focused && key_set && schedule_due(now)) {
        const char *code = info.route.valid && info.route.callsign_icao[0] != '\0'
                               ? info.route.callsign_icao : callsign;
        flight_schedule_state_t state;
        static flight_schedule_t schedule;
        memset(&schedule, 0, sizeof(schedule));
        if (!airtrack_is_airline_callsign(code)) {
            state = FLIGHT_SCHEDULE_UNSUPPORTED;
        } else if (!budget_available()) {
            state = FLIGHT_SCHEDULE_QUOTA;
        } else {
            char key[AIRTRACK_FLYSTACK_KEY_MAX_LENGTH + 1U];
            lock();
            memcpy(key, s_flight.key, sizeof(key));
            unlock();
            state = schedule_lookup(key, "flight_icao", code, &schedule);
            memset(key, 0, sizeof(key));
        }
        s_flight.schedule_attempt_ms = now;
        lock();
        if (strcmp(s_flight.info.code, info.code) == 0) {
            if (state == FLIGHT_SCHEDULE_OK) {
                s_flight.info.schedule = schedule;
                s_flight.schedule_fetch_ms = now;
                s_flight.schedule_events = false;
            }
            /* Keep good details through a failed refresh. */
            if (state == FLIGHT_SCHEDULE_OK ||
                s_flight.info.schedule_state != FLIGHT_SCHEDULE_OK) {
                s_flight.info.schedule_state = state;
            } else {
                s_flight.schedule_events = false;
            }
            ++s_flight.info.generation;
        }
        unlock();
        return;
    }

    /* The airline behind the live callsign wins over Flystack's. */
    const char *iata = info.route.airline_iata[0] != '\0' ? info.route.airline_iata
                       : aircraft != NULL && aircraft->airline_iata[0] != '\0'
                           ? aircraft->airline_iata
                       : info.schedule_state == FLIGHT_SCHEDULE_OK
                           ? info.schedule.airline_iata : "";
    if (focused && iata[0] != '\0' &&
        (!info.logo_valid || strcmp(iata, info.logo_iata) != 0)) {
        lock();
        const bool cached = strcmp(iata, s_flight.logo_pixels_iata) == 0;
        if (cached) {
            s_flight.info.logo_valid = true;
            memcpy(s_flight.info.logo_iata, iata, sizeof(s_flight.info.logo_iata));
            ++s_flight.info.generation;
        }
        unlock();
        if (!cached && (strcmp(iata, s_flight.logo_failed_iata) != 0 ||
                        now - s_flight.logo_attempt_ms > LOGO_RETRY_MS)) {
            s_flight.logo_attempt_ms = now;
            if (!logo_fetch(iata)) {
                memcpy(s_flight.logo_failed_iata, iata, sizeof(s_flight.logo_failed_iata));
            }
            return;
        }
    }

    if (usage_due) {
        char key[AIRTRACK_FLYSTACK_KEY_MAX_LENGTH + 1U];
        lock();
        memcpy(key, s_flight.key, sizeof(key));
        unlock();
        s_flight.usage_fetched_ms = now;
        usage_lookup(key);
        memset(key, 0, sizeof(key));
    }
}

esp_err_t flight_info_init(void (*wake)(void))
{
    if (s_flight.lock == NULL) {
        s_flight.lock = xSemaphoreCreateMutexStatic(&s_flight.lock_storage);
        if (s_flight.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_flight.wake = wake;
    char key[AIRTRACK_FLYSTACK_KEY_MAX_LENGTH + 1U];
    const bool key_set = airtrack_config_load_flystack_key(key, sizeof(key)) == ESP_OK;
    lock();
    s_flight.quota.key_set = key_set;
    s_flight.quota.daily_cap = FLIGHT_INFO_DAILY_CAP;
    if (key_set) {
        memcpy(s_flight.key, key, sizeof(key));
        const size_t length = strlen(key);
        (void)snprintf(s_flight.quota.key_hint, sizeof(s_flight.quota.key_hint), "%s",
                       key + (length > 4U ? length - 4U : 0U));
    }
    s_flight.info.schedule_state = key_set ? FLIGHT_SCHEDULE_IDLE : FLIGHT_SCHEDULE_NO_KEY;
    unlock();
    memset(key, 0, sizeof(key));
    return ESP_OK;
}

void flight_info_get(flight_info_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_flight.lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_flight.info;
    unlock();
}

size_t flight_info_get_trail(flight_trail_point_t *out, size_t capacity)
{
    if (out == NULL || s_flight.lock == NULL) {
        return 0U;
    }
    lock();
    const size_t count = s_flight.info.trail_count < capacity
                             ? s_flight.info.trail_count : capacity;
    memcpy(out, s_flight.trail, count * sizeof(out[0]));
    unlock();
    return count;
}

bool flight_info_copy_logo(uint16_t pixels[FLIGHT_INFO_LOGO_SIZE * FLIGHT_INFO_LOGO_SIZE],
                           uint32_t *generation)
{
    if (pixels == NULL || generation == NULL || s_flight.lock == NULL) {
        return false;
    }
    lock();
    const bool changed = s_flight.info.logo_valid &&
                         s_flight.info.logo_generation != *generation;
    if (changed) {
        memcpy(pixels, s_flight.logo, sizeof(s_flight.logo));
        *generation = s_flight.info.logo_generation;
    }
    unlock();
    return changed;
}

uint8_t *flight_info_copy_logo_png(size_t *length, char iata[3])
{
    if (length == NULL || iata == NULL || s_flight.lock == NULL) {
        return NULL;
    }
    *length = 0U;
    iata[0] = '\0';
    lock();
    uint8_t *copy = s_flight.logo_png != NULL ? malloc(s_flight.logo_png_length) : NULL;
    if (copy != NULL) {
        memcpy(copy, s_flight.logo_png, s_flight.logo_png_length);
        *length = s_flight.logo_png_length;
        memcpy(iata, s_flight.logo_pixels_iata, 3U);
    }
    unlock();
    return copy;
}

esp_err_t flight_info_request_check(const char *code, bool schedule)
{
    if (code == NULL || s_flight.lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = strnlen(code, AIRTRACK_FOCUS_MAX_LENGTH + 1U);
    if (length < 2U || length > AIRTRACK_FOCUS_MAX_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char byte = code[index];
        if (!((byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
              byte == '-' || (index == 0U && byte == '~'))) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    lock();
    memset(&s_flight.check, 0, sizeof(s_flight.check));
    s_flight.check.state = FLIGHT_CHECK_PENDING;
    memcpy(s_flight.check.query, code, length);
    s_flight.check.schedule_requested = schedule;
    unlock();
    if (s_flight.wake != NULL) {
        s_flight.wake();
    }
    return ESP_OK;
}

void flight_info_get_check(flight_check_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_flight.lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_flight.check;
    unlock();
}

esp_err_t flight_info_set_key(const char *key)
{
    if (key == NULL || s_flight.lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t result = airtrack_config_save_flystack_key(key);
    if (result != ESP_OK) {
        return result;
    }
    lock();
    memset(s_flight.key, 0, sizeof(s_flight.key));
    const size_t length = strlen(key);
    s_flight.quota.key_set = length > 0U;
    s_flight.quota.usage_valid = false;
    s_flight.quota.last_error = FLIGHT_SCHEDULE_OK;
    s_flight.quota.key_hint[0] = '\0';
    if (length > 0U) {
        (void)snprintf(s_flight.key, sizeof(s_flight.key), "%s", key);
        (void)snprintf(s_flight.quota.key_hint, sizeof(s_flight.quota.key_hint), "%s",
                       key + (length > 4U ? length - 4U : 0U));
    }
    s_flight.usage_dirty = length > 0U;
    /* A new key deserves a fresh try at whatever the old one was refused. */
    if (s_flight.info.schedule_state != FLIGHT_SCHEDULE_OK) {
        s_flight.info.schedule_state = length > 0U ? FLIGHT_SCHEDULE_IDLE
                                                   : FLIGHT_SCHEDULE_NO_KEY;
        ++s_flight.info.generation;
    }
    unlock();
    if (s_flight.wake != NULL) {
        s_flight.wake();
    }
    return ESP_OK;
}

void flight_info_get_quota(flight_quota_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_flight.lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_flight.quota;
    unlock();
}
