#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airtrack_tracker.h"

/* Host tests exercise the pure streaming tracker without linking NVS. */
esp_err_t airtrack_settings_validate(const airtrack_settings_t *settings)
{
    return settings != NULL && settings->radius_nm >= 1U &&
                   settings->radius_nm <= 250U &&
                   settings->max_position_age_s >= 5U &&
                   settings->max_position_age_s <= 120U
               ? ESP_OK
               : ESP_ERR_INVALID_ARG;
}

static airtrack_settings_t settings(bool include_ground)
{
    airtrack_settings_t value = {
        .generation = 7U,
        .location_configured = true,
        .latitude_e7 = 376213000,
        .longitude_e7 = -1223790000,
        .radius_nm = 25U,
        .poll_interval_s = 5U,
        .max_position_age_s = 15U,
        .include_ground = include_ground,
    };
    return value;
}

static esp_err_t parse_chunks(const char *json, size_t chunk,
                              bool include_ground,
                              airtrack_snapshot_t *snapshot)
{
    const airtrack_settings_t config = settings(include_ground);
    airtrack_stream_parser_t *parser = airtrack_stream_parser_create(&config);
    assert(parser != NULL);
    const size_t length = strlen(json);
    esp_err_t result = ESP_OK;
    for (size_t offset = 0U; offset < length && result == ESP_OK;
         offset += chunk) {
        size_t count = length - offset;
        if (count > chunk) {
            count = chunk;
        }
        result = airtrack_stream_parser_feed(parser, json + offset, count);
    }
    if (result == ESP_OK) {
        result = airtrack_stream_parser_finish(parser, snapshot);
    }
    airtrack_stream_parser_destroy(parser);
    return result;
}

static void test_valid_every_boundary(void)
{
    static const char response[] =
        "{\"msg\":\"ok { escaped \\\" text\",\"ac\":["
        "{\"hex\":\"abc123\",\"flight\":\" FAR1   \",\"lat\":37.70,"
        "\"lon\":-122.40,\"seen_pos\":1.0,\"dst\":7.5,\"dir\":20,"
        "\"alt_baro\":12000,\"gs\":250},"
        "{\"hex\":\"def456\",\"r\":\"N456\",\"lat\":37.65,"
        "\"lon\":-122.39,\"seen_pos\":0.5,\"dst\":2.0,\"dir\":350,"
        "\"alt_geom\":4000,\"track\":15,\"baro_rate\":-640},"
        "{\"hex\":\"aaa999\",\"lat\":37.62,\"lon\":-122.38,"
        "\"seen_pos\":1,\"dst\":0.1,\"dir\":90,\"alt_baro\":\"ground\"},"
        "{\"hex\":\"fed999\",\"lat\":37.62,\"lon\":-122.38,"
        "\"seen_pos\":99,\"dst\":0.2,\"dir\":90}],\"total\":4}";

    for (size_t chunk = 1U; chunk <= 37U; ++chunk) {
        airtrack_snapshot_t snapshot;
        assert(parse_chunks(response, chunk, false, &snapshot) == ESP_OK);
        assert(snapshot.state == AIRTRACK_FEED_LIVE);
        assert(snapshot.aircraft_reported == 4U);
        assert(snapshot.aircraft_count == 2U);
        assert(strcmp(snapshot.aircraft[0].hex, "DEF456") == 0);
        assert(strcmp(snapshot.aircraft[0].registration, "N456") == 0);
        assert(snapshot.aircraft[0].altitude_ft == 4000);
    }

    airtrack_snapshot_t ground_snapshot;
    assert(parse_chunks(response, 7U, true, &ground_snapshot) == ESP_OK);
    assert(ground_snapshot.aircraft_count == 3U);
    assert(strcmp(ground_snapshot.aircraft[0].hex, "AAA999") == 0);
    assert(ground_snapshot.aircraft[0].ground);
}

static void test_geometry_and_empty(void)
{
    static const char fallback[] =
        "{\"ac\":[{\"hex\":\"abc123\",\"lat\":37.6313,"
        "\"lon\":-122.379,\"seen_pos\":0.2,\"alt_baro\":1000}]}";
    airtrack_snapshot_t snapshot;
    assert(parse_chunks(fallback, 3U, false, &snapshot) == ESP_OK);
    assert(snapshot.aircraft_count == 1U);
    assert(snapshot.aircraft[0].distance_nm > 0.55f);
    assert(snapshot.aircraft[0].distance_nm < 0.65f);
    assert(snapshot.aircraft[0].bearing_deg < 1.0f ||
           snapshot.aircraft[0].bearing_deg > 359.0f);

    assert(parse_chunks("{\"ac\":[]}", 1U, false, &snapshot) == ESP_OK);
    assert(snapshot.state == AIRTRACK_FEED_EMPTY);
}

static void test_malformed_rejected(void)
{
    airtrack_snapshot_t snapshot;
    assert(parse_chunks("{\"ac\":[{\"hex\":\"abc123\"}]", 4U, false,
                        &snapshot) != ESP_OK);
    assert(parse_chunks("{\"ac\":[],\"ac\":[]}", 5U, false,
                        &snapshot) != ESP_OK);
    assert(parse_chunks(
               "{\"ac\":[{\"hex\":\"abc123\",\"hex\":\"def456\","
               "\"lat\":1,\"lon\":1,\"seen_pos\":1}]}",
               8U, false, &snapshot) != ESP_OK);
    assert(parse_chunks("{\"other\":[]}", 2U, false, &snapshot) != ESP_OK);
    assert(parse_chunks("{\"ac\":[,]}", 2U, false, &snapshot) != ESP_OK);
    assert(parse_chunks("{\"ac\":[{},]}", 2U, false, &snapshot) != ESP_OK);
    assert(parse_chunks("{\"ac\":[{}{}]}", 2U, false, &snapshot) != ESP_OK);

    const size_t oversized_length = 5000U;
    char *oversized = malloc(oversized_length + 128U);
    assert(oversized != NULL);
    const int prefix = snprintf(oversized, oversized_length + 128U,
        "{\"ac\":[{\"hex\":\"abc123\",\"padding\":\"");
    assert(prefix > 0);
    memset(oversized + prefix, 'a', oversized_length);
    const char suffix[] =
        "\",\"lat\":37.6,\"lon\":-122.3,\"seen_pos\":1}]}";
    memcpy(oversized + prefix + oversized_length, suffix, sizeof(suffix));
    assert(parse_chunks(oversized, 97U, false, &snapshot) ==
           ESP_ERR_INVALID_SIZE);
    free(oversized);

    const airtrack_settings_t config = settings(false);
    airtrack_stream_parser_t *parser = airtrack_stream_parser_create(&config);
    assert(parser != NULL);
    const char byte = '{';
    assert(airtrack_stream_parser_feed(
               parser, &byte, (8U * 1024U * 1024U) + 1U) ==
           ESP_ERR_INVALID_SIZE);
    airtrack_stream_parser_destroy(parser);
}

static void test_squawk_and_emergency(void)
{
    static const char response[] =
        "{\"ac\":[{\"hex\":\"abc123\",\"lat\":37.63,\"lon\":-122.38,"
        "\"seen_pos\":0.5,\"squawk\":\"7700\",\"emergency\":\"general\","
        "\"category\":\"A3\",\"alt_baro\":2000},"
        "{\"hex\":\"def456\",\"lat\":37.70,\"lon\":-122.38,"
        "\"seen_pos\":0.5,\"squawk\":\"1200\",\"emergency\":\"none\"}]}";
    airtrack_snapshot_t snapshot;
    for (size_t chunk = 1U; chunk <= 7U; chunk += 3U) {
        assert(parse_chunks(response, chunk, false, &snapshot) == ESP_OK);
        assert(snapshot.aircraft_count == 2U);
        assert(strcmp(snapshot.aircraft[0].hex, "ABC123") == 0);
        assert(strcmp(snapshot.aircraft[0].squawk, "7700") == 0);
        assert(strcmp(snapshot.aircraft[0].category, "A3") == 0);
        assert(snapshot.aircraft[0].emergency);
        assert(strcmp(snapshot.aircraft[1].squawk, "1200") == 0);
        assert(!snapshot.aircraft[1].emergency);
        assert(snapshot.aircraft[1].category[0] == '\0');
    }
    /* A duplicated consumed key still invalidates the whole poll. */
    assert(parse_chunks(
               "{\"ac\":[{\"hex\":\"abc123\",\"lat\":1,\"lon\":1,"
               "\"seen_pos\":1,\"squawk\":\"1\",\"squawk\":\"2\"}]}",
               3U, false, &snapshot) != ESP_OK);
}

static void test_target_hysteresis(void)
{
    airtrack_snapshot_t previous = {.aircraft_count = 1U};
    memcpy(previous.aircraft[0].hex, "AAAAAA", sizeof("AAAAAA"));
    previous.aircraft[0].distance_nm = 10.0f;

    airtrack_snapshot_t candidate = {.aircraft_count = 2U};
    memcpy(candidate.aircraft[0].hex, "BBBBBB", sizeof("BBBBBB"));
    candidate.aircraft[0].distance_nm = 9.5f;
    memcpy(candidate.aircraft[1].hex, "AAAAAA", sizeof("AAAAAA"));
    candidate.aircraft[1].distance_nm = 10.0f;
    char pending_hex[16] = {0};
    uint8_t pending_polls = 0U;

    airtrack_apply_target_hysteresis(&previous, &candidate, pending_hex,
                                     &pending_polls);
    assert(strcmp(candidate.aircraft[0].hex, "AAAAAA") == 0);
    assert(strcmp(pending_hex, "BBBBBB") == 0);
    assert(pending_polls == 1U);

    candidate = (airtrack_snapshot_t){.aircraft_count = 2U};
    memcpy(candidate.aircraft[0].hex, "BBBBBB", sizeof("BBBBBB"));
    candidate.aircraft[0].distance_nm = 9.4f;
    memcpy(candidate.aircraft[1].hex, "AAAAAA", sizeof("AAAAAA"));
    candidate.aircraft[1].distance_nm = 10.0f;
    airtrack_apply_target_hysteresis(&previous, &candidate, pending_hex,
                                     &pending_polls);
    assert(strcmp(candidate.aircraft[0].hex, "BBBBBB") == 0);
    assert(pending_hex[0] == '\0');
    assert(pending_polls == 0U);

    candidate = (airtrack_snapshot_t){.aircraft_count = 2U};
    memcpy(candidate.aircraft[0].hex, "CCCCCC", sizeof("CCCCCC"));
    candidate.aircraft[0].distance_nm = 8.0f;
    memcpy(candidate.aircraft[1].hex, "AAAAAA", sizeof("AAAAAA"));
    candidate.aircraft[1].distance_nm = 10.0f;
    airtrack_apply_target_hysteresis(&previous, &candidate, pending_hex,
                                     &pending_polls);
    assert(strcmp(candidate.aircraft[0].hex, "CCCCCC") == 0);
    assert(pending_hex[0] == '\0');
    assert(pending_polls == 0U);
}

static int parse_stdin(void)
{
    const airtrack_settings_t config = settings(false);
    airtrack_stream_parser_t *parser = airtrack_stream_parser_create(&config);
    assert(parser != NULL);
    char buffer[137];
    esp_err_t result = ESP_OK;
    size_t count;
    while ((count = fread(buffer, 1U, sizeof(buffer), stdin)) > 0U &&
           result == ESP_OK) {
        result = airtrack_stream_parser_feed(parser, buffer, count);
    }
    airtrack_snapshot_t snapshot;
    if (result == ESP_OK) {
        result = airtrack_stream_parser_finish(parser, &snapshot);
    }
    airtrack_stream_parser_destroy(parser);
    if (result != ESP_OK) {
        fprintf(stderr, "live parse failed: 0x%x\n", (unsigned)result);
        return 1;
    }
    printf("live parser: reports=%u accepted=%u nearest=%s %.3f NM\n",
           snapshot.aircraft_reported, snapshot.aircraft_accepted,
           snapshot.aircraft_count > 0U ? snapshot.aircraft[0].hex : "none",
           snapshot.aircraft_count > 0U
               ? (double)snapshot.aircraft[0].distance_nm : 0.0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--stdin") == 0) {
        return parse_stdin();
    }
    test_valid_every_boundary();
    test_geometry_and_empty();
    test_malformed_rejected();
    test_target_hysteresis();
    test_squawk_and_emergency();
    puts("tracker host tests: PASS");
    return 0;
}
