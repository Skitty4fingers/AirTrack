#include "flight_info.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/*
 * Parsers for the adsbdb and Flystack responses.  Everything shown on the
 * LCD or the dashboard is reduced to short printable ASCII here, whatever
 * the services send.
 */

static void copy_clean(char *out, size_t capacity, const cJSON *item, bool upper)
{
    if (capacity == 0U) {
        return;
    }
    out[0] = '\0';
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return;
    }
    const char *start = item->valuestring;
    while (*start == ' ' || *start == '\t') {
        ++start;
    }
    size_t used = 0U;
    for (const char *cursor = start; *cursor != '\0' && used + 1U < capacity; ++cursor) {
        unsigned char byte = (unsigned char)*cursor;
        if (byte >= 0x80U) {
            /* Drop multi-byte UTF-8 rather than show half a character. */
            continue;
        }
        if (byte < 0x20U || byte == 0x7fU || byte == '"' || byte == '\\' ||
            byte == '<' || byte == '>' || byte == '&') {
            byte = ' ';
        }
        out[used++] = upper ? (char)toupper(byte) : (char)byte;
    }
    while (used > 0U && out[used - 1U] == ' ') {
        --used;
    }
    out[used] = '\0';
}

/* Letters and digits only, upper-cased; used for codes that go in URLs. */
static void copy_code(char *out, size_t capacity, const cJSON *item)
{
    copy_clean(out, capacity, item, true);
    for (char *cursor = out; *cursor != '\0'; ++cursor) {
        if (!isalnum((unsigned char)*cursor)) {
            out[0] = '\0';
            return;
        }
    }
}

static const cJSON *child(const cJSON *object, const char *name)
{
    return cJSON_IsObject(object)
               ? cJSON_GetObjectItemCaseSensitive(object, name) : NULL;
}

static void airport_code(char out[5], const cJSON *airport)
{
    copy_code(out, 5U, child(airport, "iata_code"));
    if (strlen(out) != 3U) {
        copy_code(out, 5U, child(airport, "icao_code"));
    }
}

bool flight_info_parse_route(const char *body, size_t length, flight_route_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithLength(body, length);
    const cJSON *route = child(child(root, "response"), "flightroute");
    if (cJSON_IsObject(route)) {
        copy_code(out->callsign_icao, sizeof(out->callsign_icao),
                  child(route, "callsign_icao"));
        copy_code(out->callsign_iata, sizeof(out->callsign_iata),
                  child(route, "callsign_iata"));
        const cJSON *airline = child(route, "airline");
        copy_clean(out->airline_name, sizeof(out->airline_name),
                   child(airline, "name"), false);
        copy_code(out->airline_icao, sizeof(out->airline_icao), child(airline, "icao"));
        copy_code(out->airline_iata, sizeof(out->airline_iata), child(airline, "iata"));
        const cJSON *origin = child(route, "origin");
        const cJSON *destination = child(route, "destination");
        airport_code(out->origin, origin);
        airport_code(out->destination, destination);
        copy_clean(out->origin_city, sizeof(out->origin_city),
                   child(origin, "municipality"), false);
        copy_clean(out->destination_city, sizeof(out->destination_city),
                   child(destination, "municipality"), false);
        out->valid = out->callsign_icao[0] != '\0' ||
                     (out->origin[0] != '\0' && out->destination[0] != '\0');
    }
    cJSON_Delete(root);
    return out->valid;
}

static int64_t days_from_civil(int64_t year, unsigned month, unsigned day)
{
    year -= month <= 2U;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153U * (month > 2U ? month - 3U : month + 9U) + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* "YYYY-MM-DD HH:MM" in UTC -> UNIX seconds, 0 when malformed. */
static int64_t parse_utc(const cJSON *item)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return 0;
    }
    int year = 0;
    unsigned month = 0U;
    unsigned day = 0U;
    unsigned hour = 0U;
    unsigned minute = 0U;
    if (sscanf(item->valuestring, "%4d-%2u-%2u %2u:%2u", &year, &month, &day,
               &hour, &minute) != 5 ||
        year < 2000 || month < 1U || month > 12U || day < 1U || day > 31U ||
        hour > 23U || minute > 59U) {
        return 0;
    }
    return days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60;
}

static int64_t parse_time(const cJSON *object, const char *ts_name,
                          const char *utc_name)
{
    const cJSON *ts = child(object, ts_name);
    if (cJSON_IsNumber(ts) && isfinite(ts->valuedouble) &&
        ts->valuedouble > 946684800.0 && ts->valuedouble < 4102444800.0) {
        return (int64_t)ts->valuedouble;
    }
    return parse_utc(child(object, utc_name));
}

static int16_t parse_delay(const cJSON *object, const char *name)
{
    const cJSON *item = child(object, name);
    if (cJSON_IsNumber(item) && isfinite(item->valuedouble) &&
        fabs(item->valuedouble) < 10000.0) {
        return (int16_t)lround(item->valuedouble);
    }
    return FLIGHT_DELAY_UNKNOWN;
}

/*
 * The documented response is the flight object itself; tolerate it being
 * wrapped in "data" (as an object or a one-element list) as the job
 * results are, so an envelope change does not read as "not found".
 */
static const cJSON *flight_object(const cJSON *root)
{
    const cJSON *candidate = root;
    if (cJSON_IsObject(root) && child(root, "data") != NULL) {
        candidate = child(root, "data");
    }
    if (cJSON_IsArray(candidate)) {
        candidate = cJSON_GetArrayItem(candidate, 0);
    }
    return cJSON_IsObject(candidate) ? candidate : NULL;
}

flight_schedule_state_t flight_info_parse_schedule(const char *body, size_t length,
                                                   flight_schedule_t *out)
{
    memset(out, 0, sizeof(*out));
    out->dep_delay_min = FLIGHT_DELAY_UNKNOWN;
    out->arr_delay_min = FLIGHT_DELAY_UNKNOWN;
    cJSON *root = cJSON_ParseWithLength(body, length);
    if (root == NULL) {
        return FLIGHT_SCHEDULE_ERROR;
    }
    const cJSON *flight = flight_object(root);
    flight_schedule_state_t state = FLIGHT_SCHEDULE_NOT_FOUND;
    if (flight != NULL) {
        copy_clean(out->status, sizeof(out->status), child(flight, "status"), false);
        for (char *cursor = out->status; *cursor != '\0'; ++cursor) {
            *cursor = (char)tolower((unsigned char)*cursor);
        }
        copy_code(out->flight_icao, sizeof(out->flight_icao), child(flight, "flight_icao"));
        copy_code(out->flight_iata, sizeof(out->flight_iata), child(flight, "flight_iata"));
        copy_code(out->airline_iata, sizeof(out->airline_iata), child(flight, "airline_iata"));
        copy_code(out->dep_iata, sizeof(out->dep_iata), child(flight, "dep_iata"));
        if (out->dep_iata[0] == '\0') {
            copy_code(out->dep_iata, sizeof(out->dep_iata), child(flight, "dep_icao"));
        }
        copy_code(out->arr_iata, sizeof(out->arr_iata), child(flight, "arr_iata"));
        if (out->arr_iata[0] == '\0') {
            copy_code(out->arr_iata, sizeof(out->arr_iata), child(flight, "arr_icao"));
        }
        out->dep_time = parse_time(flight, "dep_time_ts", "dep_time_utc");
        out->arr_time = parse_time(flight, "arr_time_ts", "arr_time_utc");
        out->dep_delay_min = parse_delay(flight, "dep_delayed");
        out->arr_delay_min = parse_delay(flight, "arr_delayed");
        if (out->arr_delay_min == FLIGHT_DELAY_UNKNOWN) {
            out->arr_delay_min = parse_delay(flight, "delayed");
        }
        copy_clean(out->dep_terminal, sizeof(out->dep_terminal), child(flight, "dep_terminal"), true);
        copy_clean(out->dep_gate, sizeof(out->dep_gate), child(flight, "dep_gate"), true);
        copy_clean(out->arr_terminal, sizeof(out->arr_terminal), child(flight, "arr_terminal"), true);
        copy_clean(out->arr_gate, sizeof(out->arr_gate), child(flight, "arr_gate"), true);
        copy_clean(out->arr_baggage, sizeof(out->arr_baggage), child(flight, "arr_baggage"), true);
        copy_code(out->aircraft_icao, sizeof(out->aircraft_icao), child(flight, "aircraft_icao"));
        copy_clean(out->registration, sizeof(out->registration), child(flight, "reg_number"), true);
        copy_clean(out->model, sizeof(out->model), child(flight, "model"), false);
        const cJSON *built = child(flight, "built");
        if (cJSON_IsNumber(built) && built->valuedouble > 1900.0 &&
            built->valuedouble < 2200.0) {
            out->built = (int16_t)built->valuedouble;
        }
        if (out->flight_icao[0] != '\0' || out->flight_iata[0] != '\0' ||
            (out->dep_iata[0] != '\0' && out->arr_iata[0] != '\0')) {
            state = FLIGHT_SCHEDULE_OK;
        }
    }
    cJSON_Delete(root);
    return state;
}

bool flight_info_parse_usage(const char *body, size_t length, int32_t *remaining,
                             char renewal[12])
{
    renewal[0] = '\0';
    cJSON *root = cJSON_ParseWithLength(body, length);
    const cJSON *item = child(root, "remaining");
    bool ok = false;
    double value = 0.0;
    if (cJSON_IsNumber(item) && isfinite(item->valuedouble)) {
        value = item->valuedouble;
        ok = true;
    } else if (cJSON_IsString(item) && item->valuestring != NULL) {
        char *end = NULL;
        value = strtod(item->valuestring, &end);
        ok = end != item->valuestring && *end == '\0' && isfinite(value);
    }
    if (ok) {
        *remaining = value < 0.0 ? 0 : value > 2e9 ? 2000000000 : (int32_t)value;
        copy_clean(renewal, 12U, child(root, "renewal_date"), false);
    }
    cJSON_Delete(root);
    return ok;
}

const char *flight_schedule_state_name(flight_schedule_state_t state)
{
    static const char *names[] = {
        "no_key", "idle", "ok", "not_found", "unauthorized", "quota", "error",
        "unsupported",
    };
    return state <= FLIGHT_SCHEDULE_UNSUPPORTED ? names[state] : "error";
}
