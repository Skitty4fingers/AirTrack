#include "airtrack_tracker.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"

#define MAX_BODY_BYTES (8U * 1024U * 1024U)
#define MAX_OBJECT_BYTES 4096U
#define MAX_OBJECTS 8192U
#define MAX_DEPTH 16U
#define EARTH_RADIUS_NM 3440.065

struct airtrack_stream_parser {
    airtrack_settings_t settings;
    uint32_t body_bytes;
    uint32_t objects;
    uint32_t rejected;
    uint8_t depth;
    char closing[MAX_DEPTH];
    char last_root_token;
    bool root_started;
    bool root_complete;
    bool in_string;
    bool escaped;
    bool capturing_key;
    bool key_had_escape;
    char key[8];
    size_t key_length;
    bool expect_colon;
    bool pending_ac_key;
    bool expect_ac_value;
    bool seen_ac;
    bool in_ac;
    bool ac_complete;
    bool ac_expect_object;
    bool ac_any_object;
    bool capturing_object;
    char object[MAX_OBJECT_BYTES + 1U];
    size_t object_length;
    esp_err_t error;
    airtrack_aircraft_t candidates[AIRTRACK_MAX_AIRCRAFT];
    size_t candidate_count;
};

static bool finite_number(const cJSON *item)
{
    return cJSON_IsNumber(item) && isfinite(item->valuedouble);
}

static const cJSON *unique_item(const cJSON *object, const char *name,
                                bool *duplicate)
{
    const cJSON *found = NULL;
    *duplicate = false;
    for (const cJSON *item = object->child; item != NULL; item = item->next) {
        if (item->string != NULL && strcmp(item->string, name) == 0) {
            if (found != NULL) {
                *duplicate = true;
                return NULL;
            }
            found = item;
        }
    }
    return found;
}

static bool copy_display_string(char *out, size_t capacity,
                                const cJSON *item, bool trim)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL || capacity == 0U) {
        return false;
    }
    const char *start = item->valuestring;
    size_t length = strnlen(start, capacity);
    if (length >= capacity) {
        length = capacity - 1U;
    }
    if (trim) {
        while (length > 0U && isspace((unsigned char)start[length - 1U])) {
            --length;
        }
        while (length > 0U && isspace((unsigned char)*start)) {
            ++start;
            --length;
        }
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char byte = (unsigned char)start[index];
        out[index] = (byte >= 0x20U && byte != 0x7fU) ? (char)byte : '?';
    }
    out[length] = '\0';
    return length > 0U;
}

static bool valid_identity(const char *identity)
{
    const size_t length = strlen(identity);
    if (length < 3U) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char byte = (unsigned char)identity[index];
        if (!isxdigit(byte) && !(index == 0U && byte == '~')) {
            return false;
        }
    }
    return true;
}

static double radians(double degrees)
{
    return degrees * (M_PI / 180.0);
}

void airtrack_geometry(double origin_latitude, double origin_longitude,
                       double latitude, double longitude,
                       float *distance_nm, float *bearing_deg)
{
    const double lat1 = radians(origin_latitude);
    const double lat2 = radians(latitude);
    const double delta_lat = lat2 - lat1;
    const double delta_lon = radians(longitude - origin_longitude);
    const double sin_lat = sin(delta_lat / 2.0);
    const double sin_lon = sin(delta_lon / 2.0);
    double a = (sin_lat * sin_lat) +
               (cos(lat1) * cos(lat2) * sin_lon * sin_lon);
    if (a < 0.0) {
        a = 0.0;
    } else if (a > 1.0) {
        a = 1.0;
    }
    *distance_nm = (float)(EARTH_RADIUS_NM * 2.0 * atan2(sqrt(a), sqrt(1.0 - a)));
    double bearing = atan2(sin(delta_lon) * cos(lat2),
                           cos(lat1) * sin(lat2) -
                               sin(lat1) * cos(lat2) * cos(delta_lon)) *
                     (180.0 / M_PI);
    bearing = fmod(bearing + 360.0, 360.0);
    *bearing_deg = (float)bearing;
}

static int aircraft_compare(const airtrack_aircraft_t *left,
                            const airtrack_aircraft_t *right)
{
    const float difference = left->distance_nm - right->distance_nm;
    if (fabsf(difference) > 0.001f) {
        return difference < 0.0f ? -1 : 1;
    }
    if (fabsf(left->seen_pos_s - right->seen_pos_s) > 0.001f) {
        return left->seen_pos_s < right->seen_pos_s ? -1 : 1;
    }
    return strcmp(left->hex, right->hex);
}

static void insert_candidate(airtrack_stream_parser_t *parser,
                             const airtrack_aircraft_t *aircraft)
{
    size_t position = 0U;
    while (position < parser->candidate_count &&
           aircraft_compare(&parser->candidates[position], aircraft) <= 0) {
        ++position;
    }
    if (position >= AIRTRACK_MAX_AIRCRAFT) {
        return;
    }
    const size_t old_count = parser->candidate_count;
    if (parser->candidate_count < AIRTRACK_MAX_AIRCRAFT) {
        ++parser->candidate_count;
    }
    const size_t move_count = parser->candidate_count - position - 1U;
    if (move_count > 0U) {
        memmove(&parser->candidates[position + 1U],
                &parser->candidates[position],
                move_count * sizeof(parser->candidates[0]));
    }
    (void)old_count;
    parser->candidates[position] = *aircraft;
}

static esp_err_t parse_aircraft(airtrack_stream_parser_t *parser)
{
    parser->object[parser->object_length] = '\0';
    cJSON *root = cJSON_ParseWithLength(parser->object,
                                        parser->object_length + 1U);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    static const char *consumed[] = {
        "hex", "flight", "r", "t", "desc", "lat", "lon", "dst",
        "dir", "seen_pos", "alt_baro", "alt_geom", "baro_rate", "gs",
        "track", "squawk", "category", "emergency",
    };
    for (size_t index = 0U; index < sizeof(consumed) / sizeof(consumed[0]);
         ++index) {
        bool duplicate;
        (void)unique_item(root, consumed[index], &duplicate);
        if (duplicate) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    airtrack_aircraft_t aircraft = {0};
    bool duplicate;
    const cJSON *hex = unique_item(root, "hex", &duplicate);
    const cJSON *latitude = unique_item(root, "lat", &duplicate);
    const cJSON *longitude = unique_item(root, "lon", &duplicate);
    const cJSON *seen_pos = unique_item(root, "seen_pos", &duplicate);
    if (!copy_display_string(aircraft.hex, sizeof(aircraft.hex), hex, true) ||
        !valid_identity(aircraft.hex) || !finite_number(latitude) ||
        !finite_number(longitude) || !finite_number(seen_pos) ||
        latitude->valuedouble < -90.0 || latitude->valuedouble > 90.0 ||
        longitude->valuedouble < -180.0 || longitude->valuedouble > 180.0 ||
        seen_pos->valuedouble < 0.0 ||
        seen_pos->valuedouble > parser->settings.max_position_age_s) {
        cJSON_Delete(root);
        ++parser->rejected;
        return ESP_OK;
    }
    for (size_t index = 0U; aircraft.hex[index] != '\0'; ++index) {
        aircraft.hex[index] = (char)toupper((unsigned char)aircraft.hex[index]);
    }
    aircraft.latitude = latitude->valuedouble;
    aircraft.longitude = longitude->valuedouble;
    aircraft.seen_pos_s = (float)seen_pos->valuedouble;

    (void)copy_display_string(aircraft.callsign, sizeof(aircraft.callsign),
                              unique_item(root, "flight", &duplicate), true);
    (void)copy_display_string(aircraft.registration,
                              sizeof(aircraft.registration),
                              unique_item(root, "r", &duplicate), true);
    (void)copy_display_string(aircraft.aircraft_type,
                              sizeof(aircraft.aircraft_type),
                              unique_item(root, "t", &duplicate), true);
    (void)copy_display_string(aircraft.description,
                              sizeof(aircraft.description),
                              unique_item(root, "desc", &duplicate), true);

    const cJSON *altitude = unique_item(root, "alt_baro", &duplicate);
    if (cJSON_IsString(altitude) && altitude->valuestring != NULL &&
        strcmp(altitude->valuestring, "ground") == 0) {
        aircraft.ground = true;
    } else if (finite_number(altitude) &&
               altitude->valuedouble >= -2000.0 &&
               altitude->valuedouble <= 100000.0) {
        aircraft.altitude_valid = true;
        aircraft.altitude_ft = (int32_t)lround(altitude->valuedouble);
    } else {
        altitude = unique_item(root, "alt_geom", &duplicate);
        if (finite_number(altitude) && altitude->valuedouble >= -2000.0 &&
            altitude->valuedouble <= 100000.0) {
            aircraft.altitude_valid = true;
            aircraft.altitude_ft = (int32_t)lround(altitude->valuedouble);
        }
    }
    if (aircraft.ground && !parser->settings.include_ground) {
        cJSON_Delete(root);
        ++parser->rejected;
        return ESP_OK;
    }
    if (parser->settings.focus_flight[0] != '\0' &&
        !airtrack_aircraft_matches(&aircraft, parser->settings.focus_flight)) {
        cJSON_Delete(root);
        ++parser->rejected;
        return ESP_OK;
    }

    const cJSON *distance = unique_item(root, "dst", &duplicate);
    const cJSON *bearing = unique_item(root, "dir", &duplicate);
    if (finite_number(distance) && distance->valuedouble >= 0.0 &&
        distance->valuedouble <= 10000.0 && finite_number(bearing)) {
        aircraft.distance_nm = (float)distance->valuedouble;
        aircraft.bearing_deg = (float)fmod(bearing->valuedouble + 3600.0, 360.0);
    } else {
        airtrack_geometry((double)parser->settings.latitude_e7 / 10000000.0,
                          (double)parser->settings.longitude_e7 / 10000000.0,
                          aircraft.latitude, aircraft.longitude,
                          &aircraft.distance_nm, &aircraft.bearing_deg);
    }
    if (!isfinite(aircraft.distance_nm) ||
        aircraft.distance_nm > (float)parser->settings.radius_nm + 0.1f) {
        cJSON_Delete(root);
        ++parser->rejected;
        return ESP_OK;
    }

    const cJSON *speed = unique_item(root, "gs", &duplicate);
    if (finite_number(speed) && speed->valuedouble >= 0.0 &&
        speed->valuedouble <= 2000.0) {
        aircraft.ground_speed_valid = true;
        aircraft.ground_speed_kt = (float)speed->valuedouble;
    }
    const cJSON *track = unique_item(root, "track", &duplicate);
    if (finite_number(track)) {
        aircraft.track_valid = true;
        aircraft.track_deg = (float)fmod(track->valuedouble + 3600.0, 360.0);
    }
    const cJSON *rate = unique_item(root, "baro_rate", &duplicate);
    if (finite_number(rate) && fabs(rate->valuedouble) <= 20000.0) {
        aircraft.vertical_rate_valid = true;
        aircraft.vertical_rate_fpm = (int32_t)lround(rate->valuedouble);
    }
    (void)copy_display_string(aircraft.squawk, sizeof(aircraft.squawk),
                              unique_item(root, "squawk", &duplicate), true);
    (void)copy_display_string(aircraft.category, sizeof(aircraft.category),
                              unique_item(root, "category", &duplicate), true);
    const cJSON *emergency = unique_item(root, "emergency", &duplicate);
    if (cJSON_IsString(emergency) && emergency->valuestring != NULL &&
        emergency->valuestring[0] != '\0' &&
        strcmp(emergency->valuestring, "none") != 0) {
        aircraft.emergency = true;
    }

    insert_candidate(parser, &aircraft);
    cJSON_Delete(root);
    return ESP_OK;
}

airtrack_stream_parser_t *airtrack_stream_parser_create(
    const airtrack_settings_t *settings)
{
    if (settings == NULL || !settings->location_configured ||
        airtrack_settings_validate(settings) != ESP_OK) {
        return NULL;
    }
    airtrack_stream_parser_t *parser = calloc(1U, sizeof(*parser));
    if (parser != NULL) {
        parser->settings = *settings;
    }
    return parser;
}

static bool whitespace(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

static esp_err_t feed_byte(airtrack_stream_parser_t *parser, char byte)
{
    if (parser->root_complete) {
        return whitespace(byte) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    if (parser->capturing_object) {
        if (parser->object_length >= MAX_OBJECT_BYTES) {
            return ESP_ERR_INVALID_SIZE;
        }
        parser->object[parser->object_length++] = byte;
    }

    if (parser->in_string) {
        if (parser->escaped) {
            parser->escaped = false;
            if (parser->capturing_key) {
                parser->key_had_escape = true;
            }
        } else if (byte == '\\') {
            parser->escaped = true;
        } else if (byte == '"') {
            parser->in_string = false;
            if (parser->capturing_key) {
                parser->pending_ac_key = !parser->key_had_escape &&
                                         parser->key_length == 2U &&
                                         parser->key[0] == 'a' &&
                                         parser->key[1] == 'c';
                parser->expect_colon = true;
                parser->capturing_key = false;
            }
            if (parser->depth == 1U) {
                parser->last_root_token = '"';
            }
        } else if ((unsigned char)byte < 0x20U) {
            return ESP_ERR_INVALID_RESPONSE;
        } else if (parser->capturing_key && parser->key_length < sizeof(parser->key)) {
            parser->key[parser->key_length++] = byte;
        }
        return ESP_OK;
    }

    if (whitespace(byte)) {
        return ESP_OK;
    }
    if (!parser->root_started) {
        if (byte != '{') {
            return ESP_ERR_INVALID_RESPONSE;
        }
        parser->root_started = true;
    }

    if (parser->expect_colon) {
        if (byte != ':') {
            return ESP_ERR_INVALID_RESPONSE;
        }
        parser->expect_colon = false;
        parser->expect_ac_value = parser->pending_ac_key;
        parser->pending_ac_key = false;
        parser->last_root_token = ':';
        return ESP_OK;
    }
    if (parser->expect_ac_value) {
        if (byte != '[' || parser->seen_ac || parser->depth != 1U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        parser->seen_ac = true;
        parser->in_ac = true;
        parser->ac_expect_object = true;
        parser->ac_any_object = false;
        parser->expect_ac_value = false;
    }

    if (byte == '"') {
        parser->in_string = true;
        parser->escaped = false;
        parser->key_length = 0U;
        parser->key_had_escape = false;
        parser->capturing_key = parser->depth == 1U &&
            (parser->last_root_token == '{' || parser->last_root_token == ',');
        return ESP_OK;
    }

    if (parser->in_ac && parser->depth == 2U) {
        if (byte == '{') {
            if (!parser->ac_expect_object) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            parser->ac_expect_object = false;
        } else if (byte == ',') {
            if (parser->ac_expect_object || !parser->ac_any_object) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            parser->ac_expect_object = true;
        } else if (byte == ']') {
            if (parser->ac_expect_object && parser->ac_any_object) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        } else {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (byte == '{' || byte == '[') {
        if (parser->depth >= MAX_DEPTH) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (parser->in_ac && parser->depth == 2U && byte == '{') {
            parser->capturing_object = true;
            parser->object_length = 1U;
            parser->object[0] = '{';
        }
        parser->closing[parser->depth++] = byte == '{' ? '}' : ']';
    } else if (byte == '}' || byte == ']') {
        if (parser->depth == 0U || parser->closing[parser->depth - 1U] != byte) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (byte == ']' && parser->in_ac && parser->depth == 2U) {
            parser->in_ac = false;
            parser->ac_complete = true;
        }
        --parser->depth;
        if (parser->capturing_object && parser->depth == 2U && byte == '}') {
            parser->capturing_object = false;
            parser->ac_any_object = true;
            ++parser->objects;
            if (parser->objects > MAX_OBJECTS) {
                return ESP_ERR_INVALID_SIZE;
            }
            const esp_err_t result = parse_aircraft(parser);
            if (result != ESP_OK) {
                return result;
            }
        }
        if (parser->depth == 0U) {
            if (byte != '}' || !parser->root_started) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            parser->root_complete = true;
        }
    }

    if (parser->depth == 1U) {
        parser->last_root_token = byte;
    }
    return ESP_OK;
}

esp_err_t airtrack_stream_parser_feed(airtrack_stream_parser_t *parser,
                                      const char *data, size_t length)
{
    if (parser == NULL || (data == NULL && length > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (parser->error != ESP_OK) {
        return parser->error;
    }
    if (length > MAX_BODY_BYTES - parser->body_bytes) {
        parser->error = ESP_ERR_INVALID_SIZE;
        return parser->error;
    }
    parser->body_bytes += (uint32_t)length;
    for (size_t index = 0U; index < length; ++index) {
        parser->error = feed_byte(parser, data[index]);
        if (parser->error != ESP_OK) {
            return parser->error;
        }
    }
    return ESP_OK;
}

esp_err_t airtrack_stream_parser_finish(airtrack_stream_parser_t *parser,
                                        airtrack_snapshot_t *snapshot)
{
    if (parser == NULL || snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (parser->error != ESP_OK || !parser->root_complete || parser->in_string ||
        parser->depth != 0U || !parser->seen_ac || !parser->ac_complete ||
        parser->capturing_object || parser->expect_colon ||
        parser->expect_ac_value) {
        return parser->error != ESP_OK ? parser->error
                                      : ESP_ERR_INVALID_RESPONSE;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->config_generation = parser->settings.generation;
    snapshot->state = parser->candidate_count > 0U ? AIRTRACK_FEED_LIVE
                                                    : AIRTRACK_FEED_EMPTY;
    snapshot->aircraft_reported = parser->objects;
    snapshot->aircraft_accepted = (uint32_t)parser->candidate_count;
    snapshot->aircraft_rejected = parser->rejected;
    snapshot->response_bytes = parser->body_bytes;
    snapshot->aircraft_count = parser->candidate_count;
    memcpy(snapshot->aircraft, parser->candidates,
           parser->candidate_count * sizeof(parser->candidates[0]));
    return ESP_OK;
}

void airtrack_stream_parser_destroy(airtrack_stream_parser_t *parser)
{
    free(parser);
}

void airtrack_apply_target_hysteresis(const airtrack_snapshot_t *previous,
                                      airtrack_snapshot_t *candidate,
                                      char pending_hex[16],
                                      uint8_t *pending_polls)
{
    if (previous == NULL || candidate == NULL || pending_hex == NULL ||
        pending_polls == NULL || previous->aircraft_count == 0U ||
        candidate->aircraft_count < 2U ||
        strcmp(previous->aircraft[0].hex, candidate->aircraft[0].hex) == 0) {
        pending_hex[0] = '\0';
        *pending_polls = 0U;
        return;
    }
    size_t current_index = candidate->aircraft_count;
    for (size_t index = 1U; index < candidate->aircraft_count; ++index) {
        if (strcmp(previous->aircraft[0].hex, candidate->aircraft[index].hex) == 0) {
            current_index = index;
            break;
        }
    }
    if (current_index >= candidate->aircraft_count ||
        candidate->aircraft[0].distance_nm <=
            candidate->aircraft[current_index].distance_nm * 0.9f) {
        pending_hex[0] = '\0';
        *pending_polls = 0U;
        return;
    }
    if (strcmp(pending_hex, candidate->aircraft[0].hex) == 0) {
        ++*pending_polls;
    } else {
        memcpy(pending_hex, candidate->aircraft[0].hex,
               sizeof(candidate->aircraft[0].hex));
        *pending_polls = 1U;
    }
    if (*pending_polls < 2U) {
        const airtrack_aircraft_t keep = candidate->aircraft[current_index];
        memmove(&candidate->aircraft[1], &candidate->aircraft[0],
                current_index * sizeof(candidate->aircraft[0]));
        candidate->aircraft[0] = keep;
    } else {
        pending_hex[0] = '\0';
        *pending_polls = 0U;
    }
}

bool airtrack_aircraft_matches(const airtrack_aircraft_t *aircraft,
                               const char *focus)
{
    if (aircraft == NULL || focus == NULL || focus[0] == '\0') {
        return false;
    }
    return strcasecmp(aircraft->hex, focus) == 0 ||
           (aircraft->callsign[0] != '\0' &&
            strcasecmp(aircraft->callsign, focus) == 0) ||
           (aircraft->registration[0] != '\0' &&
            strcasecmp(aircraft->registration, focus) == 0);
}

const char *airtrack_feed_state_name(airtrack_feed_state_t state)
{
    static const char *names[] = {
        "config_required", "time_sync", "searching", "live",
        "empty", "stale", "offline",
    };
    return state <= AIRTRACK_FEED_OFFLINE ? names[state] : "unknown";
}

const char *airtrack_feed_error_name(airtrack_feed_error_t error)
{
    static const char *names[] = {
        "none", "wifi", "time", "dns_tls", "http", "rate_limit",
        "parse", "config",
    };
    return error <= AIRTRACK_ERROR_CONFIG ? names[error] : "unknown";
}
