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
    /* Year the airframe was built, from the ADS-B database; 0 unknown. */
    uint16_t build_year;
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
    char squawk[5];
    char category[3];
    /* True when the transponder reports any emergency other than "none". */
    bool emergency;
    /* Route enrichment (adsbdb.com), filled by the ADS-B client when known.
     * Codes are IATA when available, otherwise ICAO. */
    bool route_valid;
    char route_from[5];
    char route_to[5];
    bool destination_valid;
    double destination_latitude;
    double destination_longitude;
    bool origin_valid;
    double origin_latitude;
    double origin_longitude;
    /* Operating airline's IATA code from the same lookup; selects the logo. */
    char airline_iata[3];
    /*
     * The route database names a callsign's usual route, not today's leg:
     * flight numbers that fly out and back share one entry.  route_from/_to
     * (and the airport positions) have been put in the order ADS-B shows
     * the aircraft flying when this is true; until then the order is the
     * database's guess.
     */
    bool route_confirmed;
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

/**
 * Readable airframe name for display in place of the ICAO type designator:
 * the ADS-B database description tidied into title case ("BOEING 737 MAX 9"
 * becomes "Boeing 737 MAX 9"), else a built-in name for common designators
 * ("E75L" becomes "Embraer 175"), else "".
 */
void airtrack_airframe_name(const char *type, const char *description,
                            char *out, size_t capacity);

/** True when the aircraft's callsign, registration, or hex equals focus (case-insensitive). */
bool airtrack_aircraft_matches(const airtrack_aircraft_t *aircraft, const char *focus);

/*
 * A followed flight is requested from adsb.fi by identity rather than by
 * radius, so it stays visible from takeoff to landing wherever it is.  The
 * focus text does not say which identity it is, so the client tries the
 * plausible lookups in this order, one per poll, and keeps the one that hits.
 */
typedef enum {
    AIRTRACK_FOCUS_CALLSIGN = 0,
    AIRTRACK_FOCUS_HEX,
    AIRTRACK_FOCUS_REGISTRATION,
} airtrack_focus_kind_t;

#define AIRTRACK_FOCUS_KINDS_MAX 3U

/* A followed flight's position may be this old and still be shown; oceanic
 * and remote legs report far less often than the 15 s nearby limit. */
#define AIRTRACK_FOCUS_MAX_POSITION_AGE_S 600U

/** Fill order[] with the lookups worth trying for focus; returns the count. */
size_t airtrack_focus_lookup_order(const char *focus,
                                   airtrack_focus_kind_t order[AIRTRACK_FOCUS_KINDS_MAX]);

/** adsb.fi path segment for a lookup kind: "callsign", "hex", "registration". */
const char *airtrack_focus_kind_path(airtrack_focus_kind_t kind);

/** True for an ICAO airline callsign: three letters, then a flight number
 *  of one to four characters starting with a digit (e.g. ASA555, BAW12AB). */
bool airtrack_is_airline_callsign(const char *code);

typedef enum {
    AIRTRACK_PHASE_UNKNOWN = 0,
    AIRTRACK_PHASE_GROUND,
    AIRTRACK_PHASE_CLIMB,
    AIRTRACK_PHASE_CRUISE,
    AIRTRACK_PHASE_DESCENT,
    AIRTRACK_PHASE_APPROACH,
    AIRTRACK_PHASE_LANDED,
    AIRTRACK_PHASE_LOST,
} airtrack_flight_phase_t;

/* No position for this long reads as lost contact rather than a live phase. */
#define AIRTRACK_PHASE_LOST_AGE_S 120.0f

/**
 * Classify what a followed flight is doing.  position_age_s is the current
 * age of its last position, was_airborne whether it has been seen in the air
 * during this track, remaining_nm the distance to the destination or < 0.
 */
airtrack_flight_phase_t airtrack_flight_phase(const airtrack_aircraft_t *aircraft,
                                              float position_age_s,
                                              bool was_airborne,
                                              float remaining_nm);

const char *airtrack_flight_phase_name(airtrack_flight_phase_t phase);

/**
 * Fraction of the route flown, 0..1, from the great-circle distances to the
 * origin and the destination; < 0 when either airport position is unknown
 * or the route's direction is not yet confirmed.
 */
float airtrack_route_progress(const airtrack_aircraft_t *aircraft);

/**
 * What this position says about the direction of the route as given:
 * +1 flying origin to destination, -1 the reverse, 0 no evidence.  Climbing
 * out near an airport means leaving it and descending near one means
 * arriving; en route, the track pointing at one airport and away from the
 * other decides.  On the ground there is no evidence: taxiing out and
 * taxiing in look the same.
 */
int airtrack_route_evidence(const airtrack_aircraft_t *aircraft);

/** Swap origin and destination (codes and positions) in place. */
void airtrack_route_reverse(airtrack_aircraft_t *aircraft);

/** Great-circle distance in NM and initial bearing in degrees. */
void airtrack_geometry(double origin_latitude, double origin_longitude,
                       double latitude, double longitude,
                       float *distance_nm, float *bearing_deg);

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

