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
        .flash_bytes = 8U * 1024U * 1024U, .ssid = "CooperNet",
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
    strcpy(a->hex, "A280A4"); strcpy(a->callsign, "SKW4017");
    strcpy(a->registration, "N260SY"); strcpy(a->aircraft_type, "E75L");
    strcpy(a->squawk, "5372"); strcpy(a->category, "A3");
    a->altitude_valid = true; a->altitude_ft = 18325;
    a->vertical_rate_valid = true; a->vertical_rate_fpm = 1792;
    a->ground_speed_valid = true; a->ground_speed_kt = 364.8f;
    a->track_valid = true; a->track_deg = 110.4f;
    a->distance_nm = 1.747f; a->bearing_deg = 20.3f; a->seen_pos_s = 0.9f;

    ui_tracking_state_t state = {
        .settings = &settings, .snapshot = &snap, .ssid = "CooperNet",
        .ip_address = "10.0.0.104", .rssi_available = true, .rssi_dbm = -48,
        .wifi_connected = true,
    };
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/02_live.ppm", out_dir);
    render_and_save(path);

    /* With route enrichment. */
    a->route_valid = true; strcpy(a->route_from, "SEA"); strcpy(a->route_to, "LAX");
    a->destination_valid = true; a->destination_latitude = 33.9425; a->destination_longitude = -118.408;
    a->latitude = 47.40; a->longitude = -121.95;
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/02b_route.ppm", out_dir);
    render_and_save(path);

    /* Focused on one flight. */
    strcpy(settings.focus_flight, "SKW4017");
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/02c_focus.ppm", out_dir);
    render_and_save(path);
    snap.aircraft_count = 0; snap.state = AIRTRACK_FEED_EMPTY;
    ui_diagnostic_show_tracking(&state);
    snprintf(path, sizeof(path), "%s/02d_focus_wait.ppm", out_dir);
    render_and_save(path);
    snap.aircraft_count = 1; snap.state = AIRTRACK_FEED_LIVE;
    settings.focus_flight[0] = 0; a->route_valid = false; a->destination_valid = false;

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
    ui_diagnostic_show_setup("AirTrack-8134", "ABCD2345", "192.168.4.1", true);
    snprintf(path, sizeof(path), "%s/07_setup_recovery.ppm", out_dir);
    render_and_save(path);
    ui_diagnostic_show_setup("AirTrack-8134", "ABCD2345", "192.168.4.1", false);
    snprintf(path, sizeof(path), "%s/08_setup.ppm", out_dir);
    render_and_save(path);
    return 0;
}
