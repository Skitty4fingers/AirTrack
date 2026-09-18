/*
 * Host renderer for the AirTrack LCD screens.
 *
 * Compiles the real ui_diagnostic.c against LVGL with light stubs, drives it
 * with representative snapshots, and writes each 172x320 screen as a PPM so
 * the layout can be reviewed without the hardware.  See render.sh.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

/* Pull the unit under test in directly so static helpers are reachable. */
#include "../../components/ui/ui_diagnostic.c"

/* The Alaska logo tile as the device decodes it (32x32 RGB565 on the LCD
 * background), shared with the host tests. */
#include "../../test/host/logo_fixture.h"

static uint16_t s_frame[BOARD_LCD_H_RES * BOARD_LCD_V_RES];
static int64_t s_now_us;

int64_t esp_timer_get_time(void) { return s_now_us; }
esp_err_t board_lcd_register_color_done_callback(
    esp_lcd_panel_io_color_trans_done_cb_t cb, void *ctx)
{ (void)cb; (void)ctx; return ESP_OK; }
bool board_spi_acquire(TickType_t timeout) { (void)timeout; return true; }
void board_spi_release(void) {}
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x1,
                                    int y1, int x2, int y2, const void *data)
{
    (void)panel;
    const uint16_t *src = data;
    for (int y = y1; y < y2; ++y) {
        memcpy(&s_frame[y * BOARD_LCD_H_RES + x1], src,
               (size_t)(x2 - x1) * sizeof(uint16_t));
        src += x2 - x1;
    }
    return ESP_OK;
}
esp_err_t airtrack_settings_validate(const airtrack_settings_t *settings)
{ return settings != NULL ? ESP_OK : ESP_ERR_INVALID_ARG; }
const char *esp_err_to_name(esp_err_t code) { (void)code; return "error"; }

static void host_flush(lv_display_t *display, const lv_area_t *area,
                       uint8_t *color_map)
{
    esp_lcd_panel_draw_bitmap(NULL, area->x1, area->y1, area->x2 + 1,
                              area->y2 + 1, color_map);
    lv_display_flush_ready(display);
}

static void render_and_save(const char *path)
{
    for (int i = 0; i < 20; ++i) {
        lv_tick_inc(10);
        s_now_us += 10000;
        lv_timer_handler();
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        perror(path);
        exit(1);
    }
    fprintf(file, "P6\n%u %u\n255\n", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    for (size_t i = 0; i < BOARD_LCD_H_RES * BOARD_LCD_V_RES; ++i) {
        const uint16_t px = s_frame[i];
        const uint8_t rgb[3] = {
            (uint8_t)(((px >> 11) & 0x1f) * 255 / 31),
            (uint8_t)(((px >> 5) & 0x3f) * 255 / 63),
            (uint8_t)((px & 0x1f) * 255 / 31),
        };
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
    fprintf(stderr, "wrote %s\n", path);
}

int main(int argc, char **argv)
{
    const char *out_dir = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_display_t *display = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    static uint8_t buf[BOARD_LCD_H_RES * BOARD_LCD_STRIP_LINES * 2];
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buf, NULL, sizeof(buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, host_flush);
    lv_display_set_default(display);

    s_ui.initialized = true;
    s_ui.display = display;
    create_screen();
    s_ui.diagnostic_visible = true;
    char path[256];

    ui_diagnostic_state_t diag = {
        .phase = "Connecting to Wi-Fi", .lcd = UI_DIAGNOSTIC_OK,
        .sd = UI_DIAGNOSTIC_OK, .flash = UI_DIAGNOSTIC_OK,
        .flash_bytes = 8U * 1024U * 1024U, .ssid = "HomeNet",
    };
    ui_diagnostic_update(&diag);
    snprintf(path, sizeof(path), "%s/01_boot.ppm", out_dir);
    render_and_save(path);

    airtrack_settings_t settings = {
        .location_configured = true, .radius_nm = 25U, .poll_interval_s = 5U,
        .max_position_age_s = 15U, .distance_unit = AIRTRACK_DISTANCE_NM,
    };
    airtrack_snapshot_t snap = {0};
    snap.sequence = 10; snap.state = AIRTRACK_FEED_LIVE;
    snap.aircraft_reported = 31; snap.aircraft_accepted = 5;
    snap.updated_monotonic_ms = 1000; snap.last_success_monotonic_ms = 1000;
    s_now_us = 4000 * 1000;
    snap.aircraft_count = 1;
    airtrack_aircraft_t *a = &snap.aircraft[0];
    strcpy(a->hex, "AD9D02"); strcpy(a->callsign, "N9765M");
    strcpy(a->registration, "N9765M"); strcpy(a->aircraft_type, "M20P");
    strcpy(a->squawk, "1200"); strcpy(a->category, "A1");
    a->altitude_valid = true; a->altitude_ft = 2775;
    a->vertical_rate_valid = true; a->vertical_rate_fpm = -64;
    a->ground_speed_valid = true; a->ground_speed_kt = 94.0f;
    a->track_valid = true; a->track_deg = 28.0f;
    a->distance_nm = 8.6f; a->bearing_deg = 245.0f; a->seen_pos_s = 0.3f;

    ui_tracking_state_t state = {
        .settings = &settings, .snapshot = &snap, .ssid = "HomeNet",
        .ip_address = "192.168.1.42", .rssi_available = true, .rssi_dbm = -48,
        .wifi_connected = true,
    };
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/02_live.ppm", out_dir);
    render_and_save(path);

    /* Nearest mode with route enrichment: a different airliner. */
    strcpy(a->hex, "A6D3E1"); strcpy(a->callsign, "ASA450"); strcpy(a->registration, "N949AK");
    strcpy(a->aircraft_type, "B39M"); a->altitude_ft = 10075; a->vertical_rate_fpm = 1344;
    a->ground_speed_kt = 303.0f; a->track_deg = 53.0f; a->distance_nm = 4.1f; a->bearing_deg = 331.0f;
    a->route_valid = true; strcpy(a->route_from, "SEA"); strcpy(a->route_to, "ANC");
    a->destination_valid = true; a->destination_latitude = 61.1744; a->destination_longitude = -149.996;
    a->latitude = 47.40; a->longitude = -121.95;
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/02b_route.ppm", out_dir);
    render_and_save(path);

    /* Following one flight: ASA555 Minneapolis to Seattle, over Montana,
     * with adsbdb route, Flystack schedule (departed 12 min late), and the
     * airline logo. */
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
    const int64_t now = (int64_t)time(NULL);
    strcpy(settings.focus_flight, "ASA555");
    settings.distance_unit = AIRTRACK_DISTANCE_MI;
    static flight_info_t flight;
    memset(&flight, 0, sizeof(flight));
    strcpy(flight.code, "ASA555");
    flight.route.valid = true;
    strcpy(flight.route.callsign_icao, "ASA555"); strcpy(flight.route.callsign_iata, "AS555");
    strcpy(flight.route.airline_name, "Alaska Airlines"); strcpy(flight.route.airline_iata, "AS");
    strcpy(flight.route.origin, "MSP"); strcpy(flight.route.destination, "SEA");
    flight.schedule_state = FLIGHT_SCHEDULE_OK;
    strcpy(flight.schedule.status, "en-route");
    flight.schedule.dep_time = now - 2 * 3600 - 12 * 60; flight.schedule.dep_delay_min = 12;
    flight.schedule.arr_time = now + 70 * 60; flight.schedule.arr_delay_min = 9;
    strcpy(flight.schedule.dep_gate, "C14"); strcpy(flight.schedule.arr_gate, "N9");
    strcpy(flight.schedule.aircraft_icao, "B739");
    flight.was_airborne = true; flight.phase = AIRTRACK_PHASE_CRUISE; flight.logo_valid = true;
    state.flight = &flight;
    state.logo = LOGO_AS_EXPECTED_32; state.logo_generation = 1U;
    strcpy(a->hex, "A7E151"); strcpy(a->callsign, "ASA555"); strcpy(a->registration, "N607AS");
    strcpy(a->aircraft_type, "B739"); a->altitude_valid = true; a->altitude_ft = 36000;
    a->vertical_rate_fpm = 0; a->ground_speed_kt = 468.0f; a->track_deg = 281.0f;
    a->latitude = 46.95; a->longitude = -110.2; a->distance_nm = 486.0f; a->bearing_deg = 97.0f;
    a->route_valid = true; strcpy(a->route_from, "MSP"); strcpy(a->route_to, "SEA");
    a->origin_valid = true; a->origin_latitude = 44.882; a->origin_longitude = -93.2218;
    a->destination_valid = true; a->destination_latitude = 47.449; a->destination_longitude = -122.309;
    a->route_confirmed = true; flight.route_confirmed = true;
    strcpy(a->description, "Boeing 737-900");
    a->seen_pos_s = 0.4f; snap.last_success_monotonic_ms = 51000; s_now_us = 51000 * 1000;
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/02c_focus_cruise.ppm", out_dir);
    render_and_save(path);

    /* Before departure: nothing reported yet, schedule known. */
    flight.was_airborne = false; flight.phase = AIRTRACK_PHASE_UNKNOWN;
    strcpy(flight.schedule.status, "scheduled");
    flight.schedule.dep_time = now + 50 * 60; flight.schedule.dep_delay_min = 0;
    flight.schedule.arr_time = now + 50 * 60 + 4 * 3600; flight.schedule.arr_delay_min = 0;
    snap.aircraft_count = 0; snap.state = AIRTRACK_FEED_EMPTY;
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/02d_focus_wait.ppm", out_dir);
    render_and_save(path);

    /* On approach to Seattle, no Flystack key (adsbdb only). */
    snap.aircraft_count = 1; snap.state = AIRTRACK_FEED_LIVE;
    flight.schedule_state = FLIGHT_SCHEDULE_NO_KEY;
    flight.was_airborne = true; flight.phase = AIRTRACK_PHASE_APPROACH;
    a->altitude_ft = 6400; a->vertical_rate_fpm = -1100; a->ground_speed_kt = 214.0f;
    a->latitude = 47.72; a->longitude = -122.05; a->distance_nm = 22.0f; a->bearing_deg = 331.0f;
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/02e_focus_approach.ppm", out_dir);
    render_and_save(path);

    /* Landed; the transponder went quiet 8 minutes ago. */
    flight.phase = AIRTRACK_PHASE_LANDED;
    a->ground = true; a->altitude_valid = false; a->vertical_rate_valid = false;
    a->ground_speed_kt = 0.0f; a->latitude = 47.449; a->longitude = -122.309;
    a->distance_nm = 26.0f; a->bearing_deg = 283.0f; a->seen_pos_s = 480.0f;
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/02f_focus_landed.ppm", out_dir);
    render_and_save(path);

    state.flight = NULL; state.logo = NULL;
    a->ground = false; a->altitude_valid = true; a->vertical_rate_valid = true; a->seen_pos_s = 0.3f;
    settings.distance_unit = AIRTRACK_DISTANCE_NM;
    snap.aircraft_count = 1; snap.state = AIRTRACK_FEED_LIVE;
    settings.focus_flight[0] = 0; a->route_valid = false; a->destination_valid = false;
    a->origin_valid = false; a->route_confirmed = false; a->description[0] = 0;
    /* restore the GA-style example for the remaining scenarios */
    strcpy(a->hex, "A280A4"); strcpy(a->callsign, "SKW4017"); strcpy(a->registration, "N260SY");
    strcpy(a->aircraft_type, "E75L"); a->altitude_ft = 18325; a->vertical_rate_fpm = 1792;
    a->ground_speed_kt = 364.8f; a->track_deg = 110.4f; a->distance_nm = 1.747f; a->bearing_deg = 20.3f;

    /* Long callsign, descending, km, far away, stale. */
    strcpy(a->callsign, "N12345AB"); a->vertical_rate_fpm = -2432;
    a->distance_nm = 123.4f; a->bearing_deg = 247.0f;
    settings.distance_unit = AIRTRACK_DISTANCE_KM;
    snap.state = AIRTRACK_FEED_STALE; snap.error = AIRTRACK_ERROR_DNS_TLS;
    s_now_us = 50000 * 1000;
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/03_stale.ppm", out_dir);
    render_and_save(path);

    /* Emergency squawk. */
    strcpy(a->callsign, "DAL505"); a->emergency = true; strcpy(a->squawk, "7700");
    a->distance_nm = 8.1f; a->bearing_deg = 284.0f; a->vertical_rate_fpm = 0;
    settings.distance_unit = AIRTRACK_DISTANCE_NM;
    snap.state = AIRTRACK_FEED_LIVE; snap.error = AIRTRACK_ERROR_NONE;
    s_now_us = 51000 * 1000; snap.last_success_monotonic_ms = 50000;
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/04_emergency.ppm", out_dir);
    render_and_save(path);

    /* Empty sky. */
    snap.aircraft_count = 0; snap.state = AIRTRACK_FEED_EMPTY;
    snap.aircraft_reported = 12;
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/05_empty.ppm", out_dir);
    render_and_save(path);

    /* Wi-Fi lost. */
    snap.state = AIRTRACK_FEED_OFFLINE; snap.error = AIRTRACK_ERROR_WIFI;
    state.wifi_connected = false; state.ip_address = "--"; state.rssi_available = false;
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/06_nowifi.ppm", out_dir);
    render_and_save(path);

    /* Setup screen (recovery variant). */
    ui_diagnostic_show_setup("AirTrack-A31F", "FLY48271", "192.168.4.1", true);
    snprintf(path, sizeof(path), "%s/07_setup_recovery.ppm", out_dir);
    render_and_save(path);
    ui_diagnostic_show_setup("AirTrack-A31F", "FLY48271", "192.168.4.1", false);
    snprintf(path, sizeof(path), "%s/08_setup.ppm", out_dir);
    render_and_save(path);
    ui_diagnostic_show_updating("1.6.1", 62, "Downloading", false);
    snprintf(path, sizeof(path), "%s/09_updating.ppm", out_dir);
    render_and_save(path);
    return 0;
}
