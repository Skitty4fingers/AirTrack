#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airtrack_tracker.h"
#include "flight_info.h"
#include "flight_png.h"

#include "logo_fixture.h"

/*
 * Host inflate for the PNG tests: stored (uncompressed) deflate blocks only,
 * which is what the fixtures are written with.  On the device the ROM's
 * tinfl handles real compressed streams.
 */
esp_err_t flight_png_inflate(const uint8_t *in, size_t in_length, uint8_t *out,
                             size_t expected)
{
    if (in_length < 2U || (in[0] & 0x0fU) != 8U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t offset = 2U;
    size_t used = 0U;
    for (;;) {
        if (offset + 5U > in_length) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint8_t header = in[offset];
        if (((header >> 1) & 3U) != 0U) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        const size_t length = (size_t)in[offset + 1U] | ((size_t)in[offset + 2U] << 8);
        offset += 5U;
        if (offset + length > in_length || used + length > expected) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        memcpy(out + used, in + offset, length);
        used += length;
        offset += length;
        if ((header & 1U) != 0U) {
            break;
        }
    }
    return used == expected ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static airtrack_settings_t focus_settings(const char *focus)
{
    airtrack_settings_t settings = {
        .generation = 1U,
        .location_configured = true,
        .latitude_e7 = 474502000,
        .longitude_e7 = -1223088000,
        .radius_nm = 25U,
        .poll_interval_s = 5U,
        .max_position_age_s = 15U,
        .include_ground = false,
        .brightness_percent = 40U,
        .retention_days = 30U,
        .retention_mib = 64U,
        .sighting_window_min = 30U,
    };
    memcpy(settings.hostname, "airtrack", sizeof("airtrack"));
    (void)snprintf(settings.focus_flight, sizeof(settings.focus_flight), "%s", focus);
    return settings;
}

esp_err_t airtrack_settings_validate(const airtrack_settings_t *settings)
{
    return settings != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void test_focus_lookup_order(void)
{
    airtrack_focus_kind_t order[AIRTRACK_FOCUS_KINDS_MAX];
    assert(airtrack_focus_lookup_order("ASA555", order) == 1U);
    assert(order[0] == AIRTRACK_FOCUS_CALLSIGN);
    /* Both an airline callsign and valid hex: callsign first. */
    assert(airtrack_focus_lookup_order("ABC123", order) == 2U);
    assert(order[0] == AIRTRACK_FOCUS_CALLSIGN && order[1] == AIRTRACK_FOCUS_HEX);
    assert(airtrack_focus_lookup_order("A280A4", order) == 2U);
    assert(order[0] == AIRTRACK_FOCUS_HEX && order[1] == AIRTRACK_FOCUS_CALLSIGN);
    assert(airtrack_focus_lookup_order("N37267", order) == 2U);
    assert(order[0] == AIRTRACK_FOCUS_REGISTRATION && order[1] == AIRTRACK_FOCUS_CALLSIGN);
    assert(airtrack_focus_lookup_order("C-GABC", order) == 1U);
    assert(order[0] == AIRTRACK_FOCUS_REGISTRATION);
    assert(airtrack_focus_lookup_order("~A1B2C3", order) == 1U);
    assert(order[0] == AIRTRACK_FOCUS_HEX);
    assert(airtrack_focus_lookup_order("", order) == 0U);
    assert(strcmp(airtrack_focus_kind_path(AIRTRACK_FOCUS_REGISTRATION), "registration") == 0);

    assert(airtrack_is_airline_callsign("ASA555"));
    assert(airtrack_is_airline_callsign("BAW12AB"));
    assert(airtrack_is_airline_callsign("UAL1"));
    assert(!airtrack_is_airline_callsign("AS555"));
    assert(!airtrack_is_airline_callsign("N37267"));
    assert(!airtrack_is_airline_callsign("ASAB55"));
    assert(!airtrack_is_airline_callsign("ASA55555"));
}

static esp_err_t parse_one(const char *json, const airtrack_settings_t *settings,
                           airtrack_snapshot_t *snapshot)
{
    airtrack_stream_parser_t *parser = airtrack_stream_parser_create(settings);
    assert(parser != NULL);
    esp_err_t result = airtrack_stream_parser_feed(parser, json, strlen(json));
    if (result == ESP_OK) {
        result = airtrack_stream_parser_finish(parser, snapshot);
    }
    airtrack_stream_parser_destroy(parser);
    return result;
}

static void test_focus_ignores_radius_ground_and_short_age(void)
{
    /* Over Minneapolis, ~1,200 NM from the Seattle location, 5 minutes old,
     * no dst/dir (as the v2 identity endpoints return). */
    static const char far_away[] =
        "{\"ac\":[{\"hex\":\"a7e151\",\"flight\":\"ASA555  \",\"r\":\"N607AS\","
        "\"lat\":44.9,\"lon\":-93.2,\"seen_pos\":300,\"alt_baro\":34000}]}";
    airtrack_snapshot_t snapshot;
    airtrack_settings_t nearest = focus_settings("");
    assert(parse_one(far_away, &nearest, &snapshot) == ESP_OK);
    assert(snapshot.aircraft_count == 0U);

    airtrack_settings_t focused = focus_settings("ASA555");
    assert(parse_one(far_away, &focused, &snapshot) == ESP_OK);
    assert(snapshot.aircraft_count == 1U);
    assert(snapshot.aircraft[0].distance_nm > 1100.0f);
    assert(snapshot.aircraft[0].distance_nm < 1300.0f);
    assert(snapshot.aircraft[0].build_year == 0U);

    static const char with_year[] =
        "{\"ac\":[{\"hex\":\"a7e151\",\"flight\":\"ASA555\",\"lat\":44.9,\"lon\":-93.2,"
        "\"seen_pos\":1,\"desc\":\"BOEING 737-700\",\"year\":\"1999\"}]}";
    assert(parse_one(with_year, &focused, &snapshot) == ESP_OK);
    assert(snapshot.aircraft[0].build_year == 1999U);
    assert(strcmp(snapshot.aircraft[0].description, "Boeing 737-700") == 0);

    /* Too old even for a followed flight. */
    static const char ancient[] =
        "{\"ac\":[{\"hex\":\"a7e151\",\"flight\":\"ASA555\",\"lat\":44.9,"
        "\"lon\":-93.2,\"seen_pos\":900}]}";
    assert(parse_one(ancient, &focused, &snapshot) == ESP_OK);
    assert(snapshot.aircraft_count == 0U);

    /* At the gate: hidden by the airborne filter, shown when followed. */
    static const char at_gate[] =
        "{\"ac\":[{\"hex\":\"a7e151\",\"flight\":\"ASA555\",\"lat\":47.45,"
        "\"lon\":-122.30,\"seen_pos\":1,\"alt_baro\":\"ground\"}]}";
    assert(parse_one(at_gate, &nearest, &snapshot) == ESP_OK);
    assert(snapshot.aircraft_count == 0U);
    assert(parse_one(at_gate, &focused, &snapshot) == ESP_OK);
    assert(snapshot.aircraft_count == 1U && snapshot.aircraft[0].ground);

    /* A different flight returned by the lookup is still refused. */
    airtrack_settings_t other = focus_settings("UAL205");
    assert(parse_one(far_away, &other, &snapshot) == ESP_OK);
    assert(snapshot.aircraft_count == 0U);
}

static airtrack_aircraft_t airborne(int32_t altitude, int32_t rate, float speed)
{
    airtrack_aircraft_t aircraft = {0};
    aircraft.altitude_valid = true;
    aircraft.altitude_ft = altitude;
    aircraft.vertical_rate_valid = true;
    aircraft.vertical_rate_fpm = rate;
    aircraft.ground_speed_valid = true;
    aircraft.ground_speed_kt = speed;
    return aircraft;
}

static void test_airframe_names(void)
{
    char name[41];
    /* Descriptions as adsb.fi sends them (sampled around SEA). */
    airtrack_airframe_name("B39M", "BOEING 737 MAX 9", name, sizeof(name));
    assert(strcmp(name, "Boeing 737 MAX 9") == 0);
    airtrack_airframe_name("C180", "CESSNA  180 Skywagon", name, sizeof(name));
    assert(strcmp(name, "Cessna 180 Skywagon") == 0);
    airtrack_airframe_name("DH8D", "DE HAVILLAND DHC-8-400 Dash 8", name, sizeof(name));
    assert(strcmp(name, "De Havilland DHC-8-400 Dash 8") == 0);
    airtrack_airframe_name("A21N", "AIRBUS A-321neo", name, sizeof(name));
    assert(strcmp(name, "Airbus A-321neo") == 0);
    airtrack_airframe_name("CL35", "BOMBARDIER BD-100 Challenger 350", name, sizeof(name));
    assert(strcmp(name, "Bombardier BD-100 Challenger 350") == 0);
    /* No description: the built-in table, else nothing. */
    airtrack_airframe_name("E75L", "", name, sizeof(name));
    assert(strcmp(name, "Embraer 175") == 0);
    airtrack_airframe_name("ZZZZ", "", name, sizeof(name));
    assert(name[0] == '\0');
    /* Truncated safely, without a trailing space. */
    char tiny[8];
    airtrack_airframe_name("B39M", "BOEING 737 MAX 9", tiny, sizeof(tiny));
    assert(strcmp(tiny, "Boeing") == 0);
}

static void test_flight_phase(void)
{
    airtrack_aircraft_t aircraft = {0};
    aircraft.ground = true;
    assert(airtrack_flight_phase(&aircraft, 1.0f, false, -1.0f) == AIRTRACK_PHASE_GROUND);
    assert(airtrack_flight_phase(&aircraft, 1.0f, true, 1.0f) == AIRTRACK_PHASE_LANDED);
    /* Still landed long after the transponder went quiet. */
    assert(airtrack_flight_phase(&aircraft, 900.0f, true, 1.0f) == AIRTRACK_PHASE_LANDED);
    /* On the ground at the "destination" without a confirmed direction is
     * just on the ground: ASA555 taxiing out of SEA looked exactly like this
     * against adsbdb's MSP-SEA. */
    assert(airtrack_flight_phase(&aircraft, 1.0f, false, 0.4f) == AIRTRACK_PHASE_GROUND);
    aircraft.route_confirmed = true;
    assert(airtrack_flight_phase(&aircraft, 1.0f, false, 0.4f) == AIRTRACK_PHASE_LANDED);
    assert(airtrack_flight_phase(&aircraft, 1.0f, false, 1200.0f) == AIRTRACK_PHASE_GROUND);

    aircraft = airborne(4000, 2500, 220.0f);
    assert(airtrack_flight_phase(&aircraft, 1.0f, true, 900.0f) == AIRTRACK_PHASE_CLIMB);
    aircraft = airborne(36000, 0, 460.0f);
    assert(airtrack_flight_phase(&aircraft, 1.0f, true, 900.0f) == AIRTRACK_PHASE_CRUISE);
    assert(airtrack_flight_phase(&aircraft, 400.0f, true, 900.0f) == AIRTRACK_PHASE_LOST);
    aircraft = airborne(24000, -1800, 420.0f);
    assert(airtrack_flight_phase(&aircraft, 1.0f, true, 150.0f) == AIRTRACK_PHASE_DESCENT);
    aircraft = airborne(6000, -900, 210.0f);
    assert(airtrack_flight_phase(&aircraft, 1.0f, true, 25.0f) == AIRTRACK_PHASE_APPROACH);
    aircraft = airborne(3000, 0, 170.0f);
    assert(airtrack_flight_phase(&aircraft, 1.0f, true, 12.0f) == AIRTRACK_PHASE_APPROACH);
    /* Taxiing without a ground flag. */
    aircraft = airborne(400, 0, 18.0f);
    assert(airtrack_flight_phase(&aircraft, 1.0f, false, -1.0f) == AIRTRACK_PHASE_GROUND);
    assert(strcmp(airtrack_flight_phase_name(AIRTRACK_PHASE_APPROACH), "approach") == 0);
}

/* adsbdb's route for ASA555, MSP to SEA; today's leg flew SEA to MSP. */
static airtrack_aircraft_t msp_sea(double latitude, double longitude)
{
    airtrack_aircraft_t aircraft = {0};
    aircraft.route_valid = true;
    strcpy(aircraft.route_from, "MSP");
    strcpy(aircraft.route_to, "SEA");
    aircraft.origin_valid = true;
    aircraft.origin_latitude = 44.882;
    aircraft.origin_longitude = -93.2218;
    aircraft.destination_valid = true;
    aircraft.destination_latitude = 47.449;
    aircraft.destination_longitude = -122.309;
    aircraft.latitude = latitude;
    aircraft.longitude = longitude;
    aircraft.altitude_valid = true;
    aircraft.vertical_rate_valid = true;
    return aircraft;
}

static void test_route_evidence(void)
{
    /* Climbing through 14,000 ft 26 mi out of SEA (FlightAware, 18 Sep):
     * leaving the listed destination, so the listing is backwards. */
    airtrack_aircraft_t aircraft = msp_sea(47.55, -121.80);
    aircraft.altitude_ft = 14000;
    aircraft.vertical_rate_fpm = 2200;
    assert(airtrack_route_evidence(&aircraft) == -1);
    /* Descending into SEA: arriving where listed. */
    aircraft.altitude_ft = 9000;
    aircraft.vertical_rate_fpm = -1500;
    assert(airtrack_route_evidence(&aircraft) == 1);
    /* Climbing out of MSP. */
    aircraft = msp_sea(45.0, -93.5);
    aircraft.altitude_ft = 8000;
    aircraft.vertical_rate_fpm = 2500;
    assert(airtrack_route_evidence(&aircraft) == 1);
    /* Cruising over Montana: westbound agrees, eastbound disagrees. */
    aircraft = msp_sea(46.9, -108.5);
    aircraft.altitude_ft = 35000;
    aircraft.vertical_rate_fpm = 0;
    aircraft.track_valid = true;
    aircraft.track_deg = 282.0f;
    assert(airtrack_route_evidence(&aircraft) == 1);
    aircraft.track_deg = 98.0f;
    assert(airtrack_route_evidence(&aircraft) == -1);
    /* Northbound says nothing either way. */
    aircraft.track_deg = 5.0f;
    assert(airtrack_route_evidence(&aircraft) == 0);
    /* On the ground: taxiing out and taxiing in look the same. */
    aircraft = msp_sea(47.449, -122.309);
    aircraft.ground = true;
    assert(airtrack_route_evidence(&aircraft) == 0);

    airtrack_route_reverse(&aircraft);
    assert(strcmp(aircraft.route_from, "SEA") == 0 && strcmp(aircraft.route_to, "MSP") == 0);
    assert(aircraft.origin_latitude == 47.449 && aircraft.destination_latitude == 44.882);
}

static void test_route_progress(void)
{
    airtrack_aircraft_t aircraft = {0};
    assert(airtrack_route_progress(&aircraft) < 0.0f);
    aircraft.route_confirmed = true;
    aircraft.origin_valid = true;
    aircraft.origin_latitude = 44.882;     /* MSP */
    aircraft.origin_longitude = -93.2218;
    aircraft.destination_valid = true;
    aircraft.destination_latitude = 47.449; /* SEA */
    aircraft.destination_longitude = -122.309;
    aircraft.latitude = aircraft.origin_latitude;
    aircraft.longitude = aircraft.origin_longitude;
    assert(airtrack_route_progress(&aircraft) < 0.01f);
    aircraft.latitude = aircraft.destination_latitude;
    aircraft.longitude = aircraft.destination_longitude;
    assert(airtrack_route_progress(&aircraft) > 0.99f);
    aircraft.latitude = 46.8;
    aircraft.longitude = -107.8;
    const float middle = airtrack_route_progress(&aircraft);
    assert(middle > 0.4f && middle < 0.6f);
    /* No progress until the direction is confirmed. */
    aircraft.route_confirmed = false;
    assert(airtrack_route_progress(&aircraft) < 0.0f);
}

static void test_parse_route(void)
{
    /* Captured from api.adsbdb.com/v0/callsign/AS555 (names shortened). */
    static const char body[] =
        "{\"response\":{\"flightroute\":{\"callsign\":\"AS555\",\"callsign_icao\":\"ASA555\","
        "\"callsign_iata\":\"AS555\",\"airline\":{\"name\":\"Alaska Airlines\",\"icao\":\"ASA\","
        "\"iata\":\"AS\",\"country\":\"United States\",\"country_iso\":\"US\",\"callsign\":\"ALASKA\"},"
        "\"origin\":{\"country_iso_name\":\"US\",\"elevation\":841,\"iata_code\":\"MSP\","
        "\"icao_code\":\"KMSP\",\"latitude\":44.882,\"longitude\":-93.221802,"
        "\"municipality\":\"Minneapolis\",\"name\":\"Minneapolis\\u2013Saint Paul\"},"
        "\"destination\":{\"iata_code\":\"SEA\",\"icao_code\":\"KSEA\",\"latitude\":47.449001,"
        "\"longitude\":-122.308998,\"municipality\":\"Seattle\"}}}}";
    flight_route_t route;
    assert(flight_info_parse_route(body, strlen(body), &route));
    assert(strcmp(route.callsign_icao, "ASA555") == 0);
    assert(strcmp(route.callsign_iata, "AS555") == 0);
    assert(strcmp(route.airline_name, "Alaska Airlines") == 0);
    assert(strcmp(route.airline_iata, "AS") == 0);
    assert(strcmp(route.origin, "MSP") == 0 && strcmp(route.destination, "SEA") == 0);
    assert(strcmp(route.origin_city, "Minneapolis") == 0);

    static const char unknown[] = "{\"response\":\"unknown callsign\"}";
    assert(!flight_info_parse_route(unknown, strlen(unknown), &route));
    assert(!flight_info_parse_route("not json", 8U, &route));

    /* Markup and control characters never reach the dashboard. */
    static const char hostile[] =
        "{\"response\":{\"flightroute\":{\"callsign_icao\":\"ASA<1>\","
        "\"airline\":{\"name\":\"<b>Evil\\n&Co\"},\"origin\":{\"iata_code\":\"M\\\"P\"},"
        "\"destination\":{\"iata_code\":\"SEA\"}}}}";
    assert(!flight_info_parse_route(hostile, strlen(hostile), &route) ||
           route.callsign_icao[0] == '\0');
    assert(strchr(route.airline_name, '<') == NULL);
    assert(strchr(route.airline_name, '&') == NULL);
    assert(strchr(route.airline_name, '\n') == NULL);
    assert(route.origin[0] == '\0');
}

static void test_parse_schedule(void)
{
    /* The documented example response. */
    static const char body[] =
        "{\"hex\":\"AAB812\",\"reg_number\":\"N790AN\",\"aircraft_icao\":\"B772\",\"flag\":\"US\","
        "\"lat\":33.455017,\"lng\":-118.738312,\"alt\":10668,\"dir\":80,\"speed\":942,"
        "\"airline_icao\":\"AAL\",\"airline_iata\":\"AA\",\"flight_number\":\"6\","
        "\"flight_icao\":\"AAL6\",\"flight_iata\":\"AA6\",\"cs_airline_iata\":null,"
        "\"dep_icao\":\"PHOG\",\"dep_iata\":\"OGG\",\"dep_terminal\":null,\"dep_gate\":\"29\","
        "\"dep_time\":\"2021-07-21 18:50\",\"dep_time_ts\":1626929400,"
        "\"dep_time_utc\":\"2021-07-22 04:50\",\"arr_icao\":\"KDFW\",\"arr_iata\":\"DFW\","
        "\"arr_terminal\":\"A\",\"arr_gate\":\"A24\",\"arr_baggage\":\"A28\","
        "\"arr_time\":\"2021-07-22 07:04\",\"arr_time_ts\":1626955440,"
        "\"arr_time_utc\":\"2021-07-22 12:04\",\"duration\":434,\"delayed\":null,"
        "\"dep_delayed\":12,\"arr_delayed\":null,\"updated\":1626858778,\"status\":\"en-route\","
        "\"age\":6,\"built\":2015,\"engine\":\"jet\",\"engine_count\":\"2\","
        "\"model\":\"Airbus A321-100/200 Ceo\",\"manufacturer\":\"AIRBUS\",\"msn\":\"5938\"}";
    flight_schedule_t schedule;
    assert(flight_info_parse_schedule(body, strlen(body), &schedule) == FLIGHT_SCHEDULE_OK);
    assert(strcmp(schedule.status, "en-route") == 0);
    assert(strcmp(schedule.flight_icao, "AAL6") == 0);
    assert(strcmp(schedule.airline_iata, "AA") == 0);
    assert(strcmp(schedule.dep_iata, "OGG") == 0 && strcmp(schedule.arr_iata, "DFW") == 0);
    assert(schedule.dep_time == 1626929400 && schedule.arr_time == 1626955440);
    assert(schedule.dep_delay_min == 12);
    assert(schedule.arr_delay_min == FLIGHT_DELAY_UNKNOWN);
    assert(schedule.dep_terminal[0] == '\0' && strcmp(schedule.dep_gate, "29") == 0);
    assert(strcmp(schedule.arr_baggage, "A28") == 0);
    assert(strcmp(schedule.registration, "N790AN") == 0);
    assert(schedule.built == 2015);

    /* Wrapped variants, and the UTC string when there is no timestamp. */
    static const char wrapped[] =
        "{\"data\":[{\"flight_icao\":\"ASA555\",\"dep_iata\":\"MSP\",\"arr_iata\":\"SEA\","
        "\"dep_time_utc\":\"2026-09-18 21:05\",\"status\":\"Scheduled\"}]}";
    assert(flight_info_parse_schedule(wrapped, strlen(wrapped), &schedule) == FLIGHT_SCHEDULE_OK);
    assert(strcmp(schedule.status, "scheduled") == 0);
    assert(schedule.dep_time == 1789765500); /* calendar.timegm(2026-09-18 21:05) */
    static const char object[] = "{\"data\":{\"flight_iata\":\"AS555\"}}";
    assert(flight_info_parse_schedule(object, strlen(object), &schedule) == FLIGHT_SCHEDULE_OK);

    static const char empty[] = "{\"data\":[]}";
    assert(flight_info_parse_schedule(empty, strlen(empty), &schedule) == FLIGHT_SCHEDULE_NOT_FOUND);
    static const char error[] = "{\"error\":\"Forbidden\"}";
    assert(flight_info_parse_schedule(error, strlen(error), &schedule) == FLIGHT_SCHEDULE_NOT_FOUND);
    assert(flight_info_parse_schedule("<html>", 6U, &schedule) == FLIGHT_SCHEDULE_ERROR);
}

static void test_parse_usage(void)
{
    /* Captured from GET /v1/usage. */
    static const char body[] =
        "{\"remaining\":600,\"total_used\":0,\"renewal_date\":\"2026-10-18\","
        "\"subscription\":{\"used\":0,\"total\":100,\"remaining\":100},"
        "\"rate_limit\":{\"per_minute\":null,\"per_second\":1}}";
    int32_t remaining = 0;
    char renewal[12];
    assert(flight_info_parse_usage(body, strlen(body), &remaining, renewal));
    assert(remaining == 600 && strcmp(renewal, "2026-10-18") == 0);
    static const char documented[] = "{\"plan\":\"API\",\"remaining\":\"14823\"}";
    assert(flight_info_parse_usage(documented, strlen(documented), &remaining, renewal));
    assert(remaining == 14823);
    assert(!flight_info_parse_usage("{}", 2U, &remaining, renewal));
}

/* ---- PNG ---- */

typedef struct {
    uint8_t bytes[4096];
    size_t length;
} png_builder_t;

static void put_be32(png_builder_t *png, uint32_t value)
{
    png->bytes[png->length++] = (uint8_t)(value >> 24);
    png->bytes[png->length++] = (uint8_t)(value >> 16);
    png->bytes[png->length++] = (uint8_t)(value >> 8);
    png->bytes[png->length++] = (uint8_t)value;
}

/* CRCs are not checked by the decoder, so they are left zero. */
static void put_chunk(png_builder_t *png, const char *type, const uint8_t *body,
                      size_t length)
{
    put_be32(png, (uint32_t)length);
    memcpy(png->bytes + png->length, type, 4U);
    png->length += 4U;
    memcpy(png->bytes + png->length, body, length);
    png->length += length;
    put_be32(png, 0U);
}

static void build_png(png_builder_t *png, uint32_t width, uint32_t height,
                      uint8_t color_type, const uint8_t *palette, size_t palette_length,
                      const uint8_t *alpha, size_t alpha_length,
                      const uint8_t *scanlines, size_t scanline_length)
{
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    png->length = 0U;
    memcpy(png->bytes, signature, sizeof(signature));
    png->length = sizeof(signature);
    const uint8_t ihdr[13] = {
        (uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8), (uint8_t)width,
        (uint8_t)(height >> 24), (uint8_t)(height >> 16), (uint8_t)(height >> 8), (uint8_t)height,
        8U, color_type, 0U, 0U, 0U,
    };
    put_chunk(png, "IHDR", ihdr, sizeof(ihdr));
    if (palette != NULL) {
        put_chunk(png, "PLTE", palette, palette_length);
    }
    if (alpha != NULL) {
        put_chunk(png, "tRNS", alpha, alpha_length);
    }
    /* zlib header, one final stored block, and a (zero) Adler-32. */
    uint8_t zlib[1024];
    size_t used = 0U;
    zlib[used++] = 0x78;
    zlib[used++] = 0x01;
    zlib[used++] = 0x01;
    zlib[used++] = (uint8_t)scanline_length;
    zlib[used++] = (uint8_t)(scanline_length >> 8);
    zlib[used++] = (uint8_t)~scanline_length;
    zlib[used++] = (uint8_t)(~scanline_length >> 8);
    memcpy(zlib + used, scanlines, scanline_length);
    used += scanline_length;
    memset(zlib + used, 0, 4U);
    used += 4U;
    put_chunk(png, "IDAT", zlib, used);
    put_chunk(png, "IEND", NULL, 0U);
}

static uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint8_t paeth_ref(uint8_t a, uint8_t b, uint8_t c)
{
    const int p = a + b - c;
    const int pa = abs(p - a);
    const int pb = abs(p - b);
    const int pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static void test_png_filters_and_scaling(void)
{
    /* 4x5 opaque RGB, one row per filter type, filtered forward here. */
    enum { W = 4, H = 5, BPP = 3 };
    uint8_t image[H][W * BPP];
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W * BPP; ++x) {
            image[y][x] = (uint8_t)((x * 37 + y * 71 + (x * y) * 13) & 0xff);
        }
    }
    uint8_t scanlines[H * (W * BPP + 1)];
    for (int y = 0; y < H; ++y) {
        uint8_t *row = scanlines + y * (W * BPP + 1);
        row[0] = (uint8_t)y; /* filters 0..4 */
        for (int x = 0; x < W * BPP; ++x) {
            const uint8_t a = x >= BPP ? image[y][x - BPP] : 0U;
            const uint8_t b = y > 0 ? image[y - 1][x] : 0U;
            const uint8_t c = x >= BPP && y > 0 ? image[y - 1][x - BPP] : 0U;
            const uint8_t predictor = y == 0 ? 0U : y == 1 ? a : y == 2 ? b
                                    : y == 3 ? (uint8_t)((a + b) / 2) : paeth_ref(a, b, c);
            row[1 + x] = (uint8_t)(image[y][x] - predictor);
        }
    }
    static png_builder_t png;
    build_png(&png, W, H, 2U, NULL, 0U, NULL, 0U, scanlines, sizeof(scanlines));
    uint16_t out[W * H];
    assert(flight_png_decode_rgb565(png.bytes, png.length, out, W, H, 0U) == ESP_OK);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            assert(out[y * W + x] == rgb565(image[y][x * 3], image[y][x * 3 + 1],
                                             image[y][x * 3 + 2]));
        }
    }

    /* Palette with a transparent entry, blended onto the background. */
    const uint8_t palette[6] = {255, 0, 0, 0, 0, 255};
    const uint8_t alpha[2] = {255, 0};
    const uint8_t indexed[2 * 3] = {0, 0, 1, 0, 1, 0};
    build_png(&png, 2, 2, 3U, palette, sizeof(palette), alpha, sizeof(alpha),
              indexed, sizeof(indexed));
    uint16_t tiny[4];
    assert(flight_png_decode_rgb565(png.bytes, png.length, tiny, 2, 2, 0x00ff00U) == ESP_OK);
    assert(tiny[0] == rgb565(255, 0, 0) && tiny[1] == rgb565(0, 255, 0));
    assert(tiny[2] == rgb565(0, 255, 0) && tiny[3] == rgb565(255, 0, 0));

    /* Refusals: bad signature, truncation, oversize, 16-bit depth. */
    assert(flight_png_decode_rgb565(png.bytes + 1, png.length - 1U, tiny, 2, 2, 0U) != ESP_OK);
    assert(flight_png_decode_rgb565(png.bytes, 40U, tiny, 2, 2, 0U) != ESP_OK);
    uint8_t big[256];
    memset(big, 0, sizeof(big));
    build_png(&png, 65, 1, 0U, NULL, 0U, NULL, 0U, big, 66U);
    assert(flight_png_decode_rgb565(png.bytes, png.length, tiny, 2, 2, 0U) == ESP_ERR_NOT_SUPPORTED);
    build_png(&png, 2, 2, 2U, NULL, 0U, NULL, 0U, scanlines, 14U);
    png.bytes[8U + 8U + 8U] = 16U; /* IHDR bit depth */
    assert(flight_png_decode_rgb565(png.bytes, png.length, tiny, 2, 2, 0U) == ESP_ERR_NOT_SUPPORTED);
}

static void test_png_real_logo(void)
{
    static uint16_t out[32 * 32];
    assert(flight_png_decode_rgb565(LOGO_AS_PNG, sizeof(LOGO_AS_PNG), out, 32, 32,
                                    0x07111FU) == ESP_OK);
    size_t mismatches = 0U;
    for (size_t index = 0U; index < 32U * 32U; ++index) {
        mismatches += out[index] != LOGO_AS_EXPECTED_32[index];
    }
    assert(mismatches == 0U);
}

int main(void)
{
    test_focus_lookup_order();
    test_focus_ignores_radius_ground_and_short_age();
    test_airframe_names();
    test_flight_phase();
    test_route_evidence();
    test_route_progress();
    test_parse_route();
    test_parse_schedule();
    test_parse_usage();
    test_png_filters_and_scaling();
    test_png_real_logo();
    puts("flight tests passed");
    return 0;
}
