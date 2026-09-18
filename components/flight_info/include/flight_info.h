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

/*
 * Details for the flight being followed, gathered from three sources:
 *
 *   - adsbdb.com (free): airline, route, and the ICAO/IATA forms of the
 *     flight number.  Also answers the dashboard's number check.
 *   - Flystack (keyed, small monthly quota): schedule, status, delays,
 *     terminals, gates, and airframe details.  Spent only on the followed
 *     flight, a handful of calls per flight; see flight_info.c for the policy.
 *   - pics.avs.io: a square airline logo, decoded for the LCD.
 *
 * All network work runs inside flight_info_service(), which the ADS-B worker
 * calls after each poll, so these requests never overlap its own.
 */

#define FLIGHT_INFO_LOGO_SIZE 32U
/* Fetched at this size, served as-is to the dashboard, and box-sampled
 * down to FLIGHT_INFO_LOGO_SIZE for the LCD. */
#define FLIGHT_INFO_LOGO_WEB_SIZE 64U
#define FLIGHT_INFO_TRAIL_MAX 96U
#define FLIGHT_INFO_CODE_MAX 9U
/* Billed Flystack lookups this device makes in any rolling 24 hours. */
#define FLIGHT_INFO_DAILY_CAP 12U

typedef enum {
    FLIGHT_SCHEDULE_NO_KEY = 0,  /* no Flystack key stored */
    FLIGHT_SCHEDULE_IDLE,        /* not looked up (yet) */
    FLIGHT_SCHEDULE_OK,
    FLIGHT_SCHEDULE_NOT_FOUND,
    FLIGHT_SCHEDULE_UNAUTHORIZED, /* 401/403: key expired or not allowed */
    FLIGHT_SCHEDULE_QUOTA,        /* 402/429, or this device's budget spent */
    FLIGHT_SCHEDULE_ERROR,
    FLIGHT_SCHEDULE_UNSUPPORTED,  /* not an airline flight number */
} flight_schedule_state_t;

/* adsbdb route and airline, for a callsign. */
typedef struct {
    bool valid;
    char callsign_icao[FLIGHT_INFO_CODE_MAX];
    char callsign_iata[FLIGHT_INFO_CODE_MAX];
    char airline_name[40];
    char airline_icao[4];
    char airline_iata[3];
    char origin[5];            /* IATA when known, otherwise ICAO */
    char destination[5];
    char origin_city[32];
    char destination_city[32];
} flight_route_t;

#define FLIGHT_DELAY_UNKNOWN INT16_MIN

/* Flystack flight lookup. Times are UNIX seconds, 0 when unknown. */
typedef struct {
    char status[16];           /* "scheduled", "en-route", "landed", ... */
    char flight_icao[FLIGHT_INFO_CODE_MAX];
    char flight_iata[FLIGHT_INFO_CODE_MAX];
    char airline_iata[3];
    char dep_iata[5];
    char arr_iata[5];
    int64_t dep_time;          /* scheduled */
    int64_t arr_time;          /* scheduled */
    int16_t dep_delay_min;
    int16_t arr_delay_min;
    char dep_terminal[8];
    char dep_gate[8];
    char arr_terminal[8];
    char arr_gate[8];
    char arr_baggage[8];
    char aircraft_icao[5];
    char registration[12];
    char model[40];
    int16_t built;             /* 0 when unknown */
    int64_t fetched_epoch;
} flight_schedule_t;

typedef struct {
    float latitude;
    float longitude;
    int16_t altitude_hft;      /* hundreds of feet; -1 on the ground */
} flight_trail_point_t;

typedef struct {
    /* Focus code this describes; empty while not following a flight. */
    char code[AIRTRACK_FOCUS_MAX_LENGTH + 1U];
    flight_route_t route;
    flight_schedule_state_t schedule_state;
    flight_schedule_t schedule;
    bool was_airborne;
    airtrack_flight_phase_t phase;
    /* route is in the order ADS-B has seen the aircraft fly (see
     * airtrack_aircraft_t.route_confirmed); false: adsbdb's usual order. */
    bool route_confirmed;
    /*
     * What ADS-B itself saw, as UNIX seconds (0 when not seen).  These take
     * precedence over Flystack's timetable: takeoff and landing only when
     * the moment was actually observed, the arrival estimate from distance
     * to go and ground speed while airborne.
     */
    int64_t takeoff_epoch;
    int64_t landing_epoch;
    int64_t eta_epoch;
    uint16_t trail_count;
    /* Logo for the LCD, when one was found for the airline. */
    bool logo_valid;
    char logo_iata[3];
    uint32_t logo_generation;
    /* Bumped on every change, so readers can skip unchanged copies. */
    uint32_t generation;
} flight_info_t;

typedef enum {
    FLIGHT_CHECK_IDLE = 0,
    FLIGHT_CHECK_PENDING,
    FLIGHT_CHECK_DONE,
} flight_check_state_t;

/* The dashboard's flight-number check. */
typedef struct {
    flight_check_state_t state;
    char query[AIRTRACK_FOCUS_MAX_LENGTH + 1U];
    bool schedule_requested;
    /* adsbdb answered at all (false: network failure, try again). */
    bool route_answered;
    flight_route_t route;
    flight_schedule_state_t schedule_state;
    flight_schedule_t schedule;
} flight_check_t;

typedef struct {
    bool key_set;
    char key_hint[5];           /* last four characters of the key */
    bool usage_valid;
    int32_t remaining;          /* requests left this period, per Flystack */
    char renewal[12];           /* YYYY-MM-DD */
    uint16_t calls_today;       /* billed calls this device made, rolling day */
    uint16_t daily_cap;
    flight_schedule_state_t last_error;
} flight_quota_t;

/** Load the stored key.  wake, if given, is called when a check or a key
 *  change wants the next service cycle to run now (adsb_client_wake). */
esp_err_t flight_info_init(void (*wake)(void));

/** ADS-B worker hook (see adsb_client_set_hook). */
void flight_info_service(const airtrack_settings_t *settings,
                         const airtrack_snapshot_t *snapshot, void *context);

void flight_info_get(flight_info_t *out);

/** Copy up to capacity trail points, oldest first; returns the count. */
size_t flight_info_get_trail(flight_trail_point_t *out, size_t capacity);

/** Copy the decoded logo if its generation differs from *generation. */
bool flight_info_copy_logo(uint16_t pixels[FLIGHT_INFO_LOGO_SIZE * FLIGHT_INFO_LOGO_SIZE],
                           uint32_t *generation);

/**
 * The logo as the service sent it (PNG, FLIGHT_INFO_LOGO_WEB_SIZE square),
 * for the dashboard to load from the device itself.  Returns a malloc'd
 * copy the caller frees, or NULL when there is none.
 */
uint8_t *flight_info_copy_logo_png(size_t *length, char iata[3]);

/** Queue a check of code; schedule also spends one Flystack lookup. */
esp_err_t flight_info_request_check(const char *code, bool schedule);
void flight_info_get_check(flight_check_t *out);

/** Store (or, with "", forget) the Flystack key and refresh the quota. */
esp_err_t flight_info_set_key(const char *key);
void flight_info_get_quota(flight_quota_t *out);

const char *flight_schedule_state_name(flight_schedule_state_t state);

/* Response parsers, exposed for the host tests. */
bool flight_info_parse_route(const char *body, size_t length, flight_route_t *out);
flight_schedule_state_t flight_info_parse_schedule(const char *body, size_t length,
                                                   flight_schedule_t *out);
bool flight_info_parse_usage(const char *body, size_t length, int32_t *remaining,
                             char renewal[12]);

#ifdef __cplusplus
}
#endif
