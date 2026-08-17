#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airtrack_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIRTRACK_MAX_AIRCRAFT 5U

typedef enum {
    AIRTRACK_FEED_CONFIG_REQUIRED = 0,
    AIRTRACK_FEED_TIME_SYNC,
    AIRTRACK_FEED_SEARCHING,
    AIRTRACK_FEED_LIVE,
    AIRTRACK_FEED_EMPTY,
    AIRTRACK_FEED_STALE,
    AIRTRACK_FEED_OFFLINE,
} airtrack_feed_state_t;

typedef enum {
    AIRTRACK_ERROR_NONE = 0,
    AIRTRACK_ERROR_WIFI,
    AIRTRACK_ERROR_TIME,
    AIRTRACK_ERROR_DNS_TLS,
    AIRTRACK_ERROR_HTTP,
    AIRTRACK_ERROR_RATE_LIMIT,
    AIRTRACK_ERROR_PARSE,
    AIRTRACK_ERROR_CONFIG,
} airtrack_feed_error_t;

typedef struct {
    char hex[16];
    char callsign[16];
    char registration[16];
    char aircraft_type[9];
    char description[41];
    bool ground;
    bool altitude_valid;
    int32_t altitude_ft;
    bool vertical_rate_valid;
    int32_t vertical_rate_fpm;
    bool ground_speed_valid;
    float ground_speed_kt;
    bool track_valid;
    float track_deg;
    double latitude;
    double longitude;
    float distance_nm;
    float bearing_deg;
    float seen_pos_s;
} airtrack_aircraft_t;

typedef struct {
    uint64_t sequence;
    uint64_t config_generation;
    int64_t updated_monotonic_ms;
    int64_t last_success_monotonic_ms;
    airtrack_feed_state_t state;
    airtrack_feed_error_t error;
    int http_status;
    uint32_t response_bytes;
    uint32_t aircraft_reported;
    uint32_t aircraft_accepted;
    uint32_t aircraft_rejected;
    uint32_t retry_after_s;
    size_t aircraft_count;
    airtrack_aircraft_t aircraft[AIRTRACK_MAX_AIRCRAFT];
} airtrack_snapshot_t;

typedef struct airtrack_stream_parser airtrack_stream_parser_t;

airtrack_stream_parser_t *airtrack_stream_parser_create(
    const airtrack_settings_t *settings);
esp_err_t airtrack_stream_parser_feed(airtrack_stream_parser_t *parser,
                                      const char *data, size_t length);
esp_err_t airtrack_stream_parser_finish(airtrack_stream_parser_t *parser,
                                        airtrack_snapshot_t *snapshot);
void airtrack_stream_parser_destroy(airtrack_stream_parser_t *parser);

/** Replace candidate[0] with the stable target when switch confirmation applies. */
void airtrack_apply_target_hysteresis(const airtrack_snapshot_t *previous,
                                      airtrack_snapshot_t *candidate,
                                      char pending_hex[16],
                                      uint8_t *pending_polls);

const char *airtrack_feed_state_name(airtrack_feed_state_t state);
const char *airtrack_feed_error_name(airtrack_feed_error_t error);

#ifdef __cplusplus
}
#endif

