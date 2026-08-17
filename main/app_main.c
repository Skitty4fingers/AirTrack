#include "airtrack_config.h"
#include "adsb_client.h"
#include "board.h"
#include "captive_dns.h"
#include "connectivity.h"
#include "setup_web.h"
#include "status_web.h"
#include "storage_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_diagnostic.h"
#include "mdns.h"

#define WIFI_RECOVERY_DELAY_MS 60000U
#define SUPERVISOR_INTERVAL_MS 250U
#define SETTINGS_RESTART_DELAY_MS 2500U
#define SETTINGS_RESTART_STACK_BYTES 2048U
#define STATUS_WEB_RETRY_MS 5000U
#define UI_TRACKING_INTERVAL_MS 1000U
#define RSSI_REFRESH_INTERVAL_MS 5000U
#define BOOT_SETUP_HOLD_MS 5000U
#define RECOVERY_RECONNECT_DELAY_MS 20000U
#define RECOVERY_STABLE_MS 5000U
#define VALID_TIME_EPOCH 1704067200L

static const char *TAG = "airtrack";
static portMUX_TYPE s_restart_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_restart_scheduled;
static StaticTask_t s_restart_task_storage;
static StackType_t s_restart_task_stack[
    SETTINGS_RESTART_STACK_BYTES / sizeof(StackType_t)];

static board_status_t s_board_status;
static uint32_t s_flash_bytes;
static airtrack_config_t s_config;
static airtrack_settings_t s_settings;
static bool s_mdns_started;
static bool s_sntp_started;
static bool s_tracking_started;
static connectivity_network_t
    s_scanned_networks[CONNECTIVITY_SCAN_MAX_RESULTS];
static setup_web_network_t s_web_networks[SETUP_WEB_MAX_NETWORKS];

typedef enum {
    /* No credentials: stay in isolated setup until the user saves some. */
    SETUP_REASON_UNCONFIGURED = 0,
    /* User held BOOT: isolated setup; a second hold restarts normally. */
    SETUP_REASON_BUTTON,
    /* Saved Wi-Fi unavailable: setup AP plus a throttled station retry;
     * returns to tracking automatically once the network is back. */
    SETUP_REASON_RECOVERY,
} setup_reason_t;

_Static_assert(CONNECTIVITY_SCAN_MAX_RESULTS == SETUP_WEB_MAX_NETWORKS,
               "Connectivity and setup web scan capacities must match");
_Static_assert(SETTINGS_RESTART_STACK_BYTES % sizeof(StackType_t) == 0U,
               "Restart task stack must align to StackType_t");

static void restart_after_settings_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(SETTINGS_RESTART_DELAY_MS));
    esp_restart();
}

static bool claim_settings_restart(void)
{
    taskENTER_CRITICAL(&s_restart_lock);
    if (s_restart_scheduled) {
        taskEXIT_CRITICAL(&s_restart_lock);
        return false;
    }
    s_restart_scheduled = true;
    taskEXIT_CRITICAL(&s_restart_lock);
    return true;
}

static void release_settings_restart(void)
{
    taskENTER_CRITICAL(&s_restart_lock);
    s_restart_scheduled = false;
    taskEXIT_CRITICAL(&s_restart_lock);
}

static esp_err_t launch_settings_restart(void)
{
    const TaskHandle_t task = xTaskCreateStatic(
        restart_after_settings_task, "settings_restart",
        SETTINGS_RESTART_STACK_BYTES, NULL, tskIDLE_PRIORITY + 2,
        s_restart_task_stack, &s_restart_task_storage);
    return task != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

/*
 * Settings are read by the supervisor every cycle and written by the LAN
 * dashboard from the HTTP task, so hand-offs go through a short critical
 * section and the supervisor works from a per-cycle copy.
 */
static portMUX_TYPE s_settings_lock = portMUX_INITIALIZER_UNLOCKED;

static airtrack_settings_t settings_snapshot(void)
{
    airtrack_settings_t copy;
    taskENTER_CRITICAL(&s_settings_lock);
    copy = s_settings;
    taskEXIT_CRITICAL(&s_settings_lock);
    return copy;
}

static void settings_publish(const airtrack_settings_t *updated)
{
    taskENTER_CRITICAL(&s_settings_lock);
    s_settings = *updated;
    taskEXIT_CRITICAL(&s_settings_lock);
}

static void apply_timezone(const airtrack_settings_t *settings);
static int local_minutes_of_day(void);
static bool night_now(const airtrack_settings_t *settings);

static esp_err_t save_device_settings(const setup_web_submission_t *submission,
                                      void *user_context)
{
    (void)user_context;
    if (submission == NULL || !claim_settings_restart()) {
        return ESP_ERR_INVALID_STATE;
    }

    const airtrack_settings_t current = settings_snapshot();
    airtrack_settings_t updated = submission->settings;
    /* Hostname and retention are not user-editable from the form. */
    memcpy(updated.hostname, current.hostname, sizeof(updated.hostname));
    updated.retention_days = current.retention_days;
    updated.retention_mib = current.retention_mib;
    updated.log_heartbeat_s = current.log_heartbeat_s;
    updated.max_position_age_s = current.max_position_age_s;
    esp_err_t err = airtrack_settings_validate(&updated);
    if (err == ESP_OK) {
        err = airtrack_config_save_settings(&updated);
    }
    if (err == ESP_OK) {
        err = airtrack_config_save_wifi(submission->ssid,
                                        submission->password);
    }
    if (err == ESP_OK) {
        err = launch_settings_restart();
    }

    if (err != ESP_OK) {
        release_settings_restart();
        return err;
    }

    ESP_LOGI(TAG, "Wi-Fi and tracker settings saved; restart scheduled");
    return ESP_OK;
}

/*
 * Apply tracker/display settings posted from the LAN dashboard without a
 * restart: persist first, then push the record to every consumer.  Only
 * user-editable fields are taken from the request; hostname and retention
 * limits keep their stored values.
 */
static esp_err_t save_dashboard_settings(airtrack_settings_t *requested,
                                         void *user_context)
{
    (void)user_context;
    if (requested == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    airtrack_settings_t updated = settings_snapshot();
    updated.location_configured = requested->location_configured;
    updated.latitude_e7 = requested->latitude_e7;
    updated.longitude_e7 = requested->longitude_e7;
    updated.radius_nm = requested->radius_nm;
    updated.poll_interval_s = requested->poll_interval_s;
    updated.include_ground = requested->include_ground;
    updated.distance_unit = requested->distance_unit;
    updated.brightness_percent = requested->brightness_percent;
    updated.logging_mode = requested->logging_mode;
    updated.retention_mib = requested->retention_mib;
    updated.sighting_window_min = requested->sighting_window_min;
    memcpy(updated.focus_flight, requested->focus_flight,
           sizeof(updated.focus_flight));
    updated.night_enabled = requested->night_enabled;
    updated.night_start_min = requested->night_start_min;
    updated.night_end_min = requested->night_end_min;
    updated.night_brightness_percent = requested->night_brightness_percent;
    updated.night_led_off = requested->night_led_off;
    memcpy(updated.timezone, requested->timezone, sizeof(updated.timezone));
    esp_err_t err = airtrack_settings_validate(&updated);
    if (err == ESP_OK) {
        err = airtrack_config_save_settings(&updated);
    }
    if (err != ESP_OK) {
        return err;
    }
    airtrack_settings_t stored;
    if (airtrack_config_load_settings(&stored) == ESP_OK) {
        updated = stored; /* pick up the new generation number */
    }
    settings_publish(&updated);
    *requested = updated;
    apply_timezone(&updated);
    /* Brightness is applied by the supervisor cycle (day/night aware). */
    (void)adsb_client_update_settings(&updated);
    (void)storage_logger_update_settings(&updated);
    ESP_LOGI(TAG, "Dashboard settings applied (generation %llu)",
             (unsigned long long)updated.generation);
    return ESP_OK;
}

/*
 * Factory reset: wipe the sighting log, then every stored setting (including
 * the setup-hotspot identity), then restart.  Runs in the HTTP task; the
 * restart happens after the response has been sent.
 */
static esp_err_t factory_reset_from_dashboard(void *user_context)
{
    (void)user_context;
    if (!claim_settings_restart()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t deleted = 0U;
    const esp_err_t logs = storage_logger_clear(&deleted);
    if (logs != ESP_OK && logs != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Factory reset: log clear returned %s", esp_err_to_name(logs));
    }
    esp_err_t err = airtrack_config_factory_reset();
    if (err == ESP_OK) {
        err = launch_settings_restart();
    }
    if (err != ESP_OK) {
        release_settings_restart();
        return err;
    }
    ESP_LOGW(TAG, "Factory reset requested from the dashboard; restarting");
    return ESP_OK;
}

static esp_err_t reboot_from_dashboard(void *user_context)
{
    (void)user_context;
    if (!claim_settings_restart()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = launch_settings_restart();
    if (err != ESP_OK) {
        release_settings_restart();
        return err;
    }
    ESP_LOGI(TAG, "Restart requested from the dashboard");
    return ESP_OK;
}

static ui_diagnostic_state_t make_diagnostic(const char *phase,
                                             const char *ssid,
                                             const char *ip_address)
{
    return (ui_diagnostic_state_t) {
        .phase = phase,
        .lcd = s_board_status.lcd_ready ? UI_DIAGNOSTIC_OK
                                        : UI_DIAGNOSTIC_ERROR,
        .sd = s_board_status.sd_mounted ? UI_DIAGNOSTIC_OK
                                        : UI_DIAGNOSTIC_WARNING,
        .flash = s_flash_bytes == (8U * 1024U * 1024U)
                     ? UI_DIAGNOSTIC_OK
                     : UI_DIAGNOSTIC_WARNING,
        .flash_bytes = s_flash_bytes,
        .ssid = ssid,
        .ip_address = ip_address,
    };
}

static size_t scan_nearby_networks(void)
{
    size_t network_count = 0U;
    const esp_err_t result = connectivity_scan_networks(
        s_scanned_networks, CONNECTIVITY_SCAN_MAX_RESULTS, &network_count);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Nearby Wi-Fi scan unavailable: %s",
                 esp_err_to_name(result));
        return 0U;
    }

    for (size_t index = 0U; index < network_count; ++index) {
        memcpy(s_web_networks[index].ssid, s_scanned_networks[index].ssid,
               sizeof(s_web_networks[index].ssid));
        s_web_networks[index].rssi =
            s_scanned_networks[index].rssi_dbm;
        s_web_networks[index].secured =
            s_scanned_networks[index].secured;
    }
    return network_count;
}

static status_web_snapshot_t make_status_web_snapshot(
    const connectivity_status_t *status,
    const airtrack_snapshot_t *aircraft,
    const airtrack_settings_t *settings)
{
    adsb_client_stats_t stats = {0};
    (void)adsb_client_get_stats(&stats);
    storage_logger_status_t logger = {0};
    (void)storage_logger_get_status(&logger);
    return (status_web_snapshot_t) {
        .ssid = status->ssid,
        .ip_address = status->ip_address,
        .rssi_available = status->rssi_available,
        .rssi_dbm = status->rssi_dbm,
        .sd_mounted = s_board_status.sd_mounted,
        .sd_logging_enabled = logger.enabled,
        .sd_records_written = logger.records_written,
        .sd_log_bytes = logger.log_bytes,
        .sd_log_files = logger.log_files,
        .sd_files_pruned = logger.files_pruned,
        .flash_bytes = s_flash_bytes,
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
        .free_heap_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT),
        .minimum_free_heap_bytes = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
        .time_synchronized = time(NULL) >= VALID_TIME_EPOCH,
        .night = night_now(settings),
        .local_minutes = local_minutes_of_day(),
        .polls_ok = stats.polls_ok,
        .polls_failed = stats.polls_failed,
        .tls_connections = stats.connections,
        .settings = settings,
        .aircraft = aircraft,
    };
}

static void start_time_sync(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    s_sntp_started = true;
}

static void start_mdns(void)
{
    if (s_mdns_started || mdns_init() != ESP_OK) {
        return;
    }
    if (mdns_hostname_set(s_settings.hostname) == ESP_OK &&
        mdns_instance_name_set("AirTrack aircraft display") == ESP_OK) {
        (void)mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        s_mdns_started = true;
        ESP_LOGI(TAG, "mDNS ready at http://%s.local", s_settings.hostname);
    } else {
        mdns_free();
    }
}

static esp_err_t stop_status_web_if_running(void)
{
    return status_web_is_running() ? status_web_stop() : ESP_OK;
}

static void set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_board_status.rgb_ready) {
        (void)board_rgb_set(red, green, blue);
    }
}

/*
 * The accessory LED mirrors the display's colour language: blue while the
 * feed is healthy (live or a clean empty sky), orange for anything that needs
 * attention (connecting, stale, offline, setup).  Kept dim; it is a status
 * light, not a lamp.
 */
typedef enum { LED_UNSET = 0, LED_BLUE, LED_ORANGE, LED_OFF } led_state_t;

static void set_status_led(led_state_t *current, led_state_t wanted)
{
    if (*current == wanted) {
        return;
    }
    *current = wanted;
    if (wanted == LED_BLUE) {
        set_rgb(0, 4, 12);
    } else if (wanted == LED_ORANGE) {
        set_rgb(12, 4, 0);
    } else if (s_board_status.rgb_ready) {
        (void)board_rgb_clear();
    }
}

static void apply_timezone(const airtrack_settings_t *settings)
{
    if (settings->timezone[0] != '\0') {
        setenv("TZ", settings->timezone, 1);
    } else {
        setenv("TZ", "UTC0", 1);
    }
    tzset();
}

/* Local minutes-of-day, or -1 until the clock is valid. */
static int local_minutes_of_day(void)
{
    const time_t now = time(NULL);
    if (now < VALID_TIME_EPOCH) {
        return -1;
    }
    struct tm local;
    if (localtime_r(&now, &local) == NULL) {
        return -1;
    }
    return local.tm_hour * 60 + local.tm_min;
}

/*
 * Night mode: dim the panel to the night brightness and, if configured, blank
 * the accessory LED.  Re-evaluated every supervisor cycle; the backlight is
 * only touched when the effective level changes.
 */
static bool night_now(const airtrack_settings_t *settings)
{
    return airtrack_settings_is_night(settings, local_minutes_of_day());
}

static void apply_brightness(const airtrack_settings_t *settings, bool night,
                             int *applied_percent)
{
    const int wanted = night ? settings->night_brightness_percent
                             : settings->brightness_percent;
    if (wanted != *applied_percent) {
        *applied_percent = wanted;
        (void)board_backlight_set((uint8_t)wanted);
    }
}

static bool interval_elapsed(TickType_t started, uint32_t interval_ms)
{
    return (TickType_t)(xTaskGetTickCount() - started) >=
           pdMS_TO_TICKS(interval_ms);
}

/*
 * Five-second BOOT hold detector shared by both modes.  It arms only after
 * the button has been seen released, so the hold that entered setup mode
 * cannot immediately count again as a request to leave it.
 */
typedef struct {
    bool armed;
    bool active;
    bool fired;
    TickType_t since;
} boot_hold_t;

static bool boot_hold_triggered(boot_hold_t *hold)
{
    if (!board_boot_button_is_pressed()) {
        hold->armed = true;
        hold->active = false;
        hold->fired = false;
        return false;
    }
    if (!hold->armed) {
        return false;
    }
    if (!hold->active) {
        hold->active = true;
        hold->since = xTaskGetTickCount();
        return false;
    }
    if (!hold->fired && interval_elapsed(hold->since, BOOT_SETUP_HOLD_MS)) {
        hold->fired = true;
        return true;
    }
    return false;
}

static esp_err_t enter_setup_mode(setup_reason_t reason)
{
    esp_err_t err = stop_status_web_if_running();
    if (err != ESP_OK) {
        return err;
    }
    (void)adsb_client_set_online(false);
    (void)storage_logger_stop();

    err = connectivity_init();
    if (err != ESP_OK) {
        return err;
    }

    connectivity_status_t connectivity_status;
    err = connectivity_get_status(&connectivity_status);
    if (err != ESP_OK) {
        return err;
    }
    if (connectivity_status.station_enabled) {
        /* Stop the station either way: a clean scan needs an idle radio, and
         * recovery re-adds it below with a gentle retry cadence. */
        err = connectivity_stop_station();
        if (err != ESP_OK) {
            return err;
        }
    }

    const size_t nearby_network_count = scan_nearby_networks();

    err = connectivity_start_softap(s_config.ap_ssid, s_config.ap_password);
    if (err != ESP_OK) {
        return err;
    }

    const airtrack_settings_t settings = settings_snapshot();
    const setup_web_config_t web_config = {
        .ap_ssid = s_config.ap_ssid,
        .ap_password = s_config.ap_password,
        .ap_ip_address = CONNECTIVITY_SOFTAP_IPV4,
        .nearby_networks = s_web_networks,
        .nearby_network_count = nearby_network_count,
        .current_ssid = s_config.wifi_configured ? s_config.wifi_ssid : NULL,
        .current_settings = &settings,
        .save_config = save_device_settings,
    };
    if (!setup_web_is_running()) {
        err = setup_web_start(&web_config);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (!captive_dns_is_running()) {
        err = captive_dns_start(CONNECTIVITY_SOFTAP_IPV4);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS unavailable: %s; setup remains at %s",
                     esp_err_to_name(err), CONNECTIVITY_SOFTAP_IPV4);
        }
    }

    err = ui_diagnostic_show_setup(s_config.ap_ssid, s_config.ap_password,
                                   CONNECTIVITY_SOFTAP_IPV4,
                                   reason == SETUP_REASON_RECOVERY);
    if (err != ESP_OK) {
        return err;
    }
    s_tracking_started = false;

    if (reason == SETUP_REASON_RECOVERY) {
        connectivity_set_reconnect_delay(RECOVERY_RECONNECT_DELAY_MS);
        err = connectivity_start_station(s_config.wifi_ssid,
                                         s_config.wifi_password);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Recovery station retry unavailable: %s",
                     esp_err_to_name(err));
        }
    }

    set_rgb(12, 4, 0);
    ESP_LOGI(TAG, "Setup mode ready on %s (%s)", s_config.ap_ssid,
             reason == SETUP_REASON_RECOVERY ? "recovery, station retrying"
             : reason == SETUP_REASON_BUTTON ? "requested by BOOT"
                                             : "no saved network");
    return ESP_OK;
}

static esp_err_t leave_setup_mode(void)
{
    esp_err_t first_error = ESP_OK;
    esp_err_t err = captive_dns_is_running() ? captive_dns_stop() : ESP_OK;
    if (err != ESP_OK) {
        first_error = err;
    }
    err = setup_web_is_running() ? setup_web_stop() : ESP_OK;
    if (err != ESP_OK && first_error == ESP_OK) {
        first_error = err;
    }
    err = connectivity_stop_softap();
    if (err != ESP_OK && first_error == ESP_OK) {
        first_error = err;
    }
    connectivity_set_reconnect_delay(CONNECTIVITY_DEFAULT_RECONNECT_DELAY_MS);
    const airtrack_settings_t settings = settings_snapshot();
    err = storage_logger_start(s_board_status.sd_mounted, &settings);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE &&
        first_error == ESP_OK) {
        first_error = err;
    }
    ESP_LOGI(TAG, "Setup mode closed; resuming tracking");
    return first_error;
}

/*
 * Run setup mode.  Returns only for SETUP_REASON_RECOVERY, once the saved
 * station has held an address for RECOVERY_STABLE_MS.  Other reasons end in
 * a restart (BOOT hold or a saved form).
 */
static void run_setup_mode(setup_reason_t reason)
{
    boot_hold_t boot_hold = {0};
    bool station_seen_connected = false;
    TickType_t connected_since = 0U;
    for (;;) {
        connectivity_status_t status;
        ESP_ERROR_CHECK(connectivity_get_status(&status));

        if (reason == SETUP_REASON_RECOVERY) {
            if (status.connected) {
                if (!station_seen_connected) {
                    station_seen_connected = true;
                    connected_since = xTaskGetTickCount();
                    ESP_LOGI(TAG, "Saved network is back (%s); confirming",
                             status.ip_address);
                } else if (interval_elapsed(connected_since,
                                            RECOVERY_STABLE_MS)) {
                    return;
                }
            } else {
                station_seen_connected = false;
            }
        }

        if (boot_hold_triggered(&boot_hold)) {
            ESP_LOGI(TAG, "BOOT hold in setup mode; restarting");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_INTERVAL_MS));
    }
}

/*
 * Run normal tracking until either the BOOT button requests setup or the
 * station has been unavailable for WIFI_RECOVERY_DELAY_MS.
 */
static setup_reason_t run_tracking_mode(void)
{
    TickType_t disconnected_since = xTaskGetTickCount();
#ifdef AIRTRACK_TEST_FORCE_RECOVERY_MS
    /* Hardware test hook (see main/CMakeLists.txt): pretend the station was
     * lost once, so the recovery AP+STA round trip can be exercised on a bench
     * with the router still present. */
    static bool forced_once;
    const TickType_t entered = xTaskGetTickCount();
#endif
    TickType_t last_status_web_attempt =
        xTaskGetTickCount() - pdMS_TO_TICKS(STATUS_WEB_RETRY_MS);
    TickType_t last_rssi_refresh = xTaskGetTickCount();
    bool last_connected = false;
    bool have_last = false;
    char last_ip[CONNECTIVITY_IPV4_TEXT_MAX_BYTES + 1U] = {0};
    uint64_t last_snapshot_sequence = UINT64_MAX;
    TickType_t last_ui_update = 0U;
    TickType_t last_web_update = 0U;
    boot_hold_t boot_hold = {0};
    led_state_t led = LED_UNSET;
    int applied_brightness = -1;
    bool last_night = false;

    for (;;) {
        connectivity_status_t status;
        ESP_ERROR_CHECK(connectivity_get_status(&status));
        const airtrack_settings_t settings = settings_snapshot();
        const bool night = night_now(&settings);
        if (night != last_night) {
            last_night = night;
            ESP_LOGI(TAG, "%s mode (local %02d:%02d)", night ? "Night" : "Day",
                     local_minutes_of_day() / 60, local_minutes_of_day() % 60);
        }
        apply_brightness(&settings, night, &applied_brightness);

        const bool status_changed = !have_last ||
                                    status.connected != last_connected ||
                                    strcmp(status.ip_address, last_ip) != 0;
        if (status_changed) {
            have_last = true;
            if (!s_tracking_started) {
                const ui_diagnostic_state_t network_state = make_diagnostic(
                    status.connected ? "Wi-Fi connected" : "Connecting to Wi-Fi",
                    status.ssid[0] != '\0' ? status.ssid : s_config.wifi_ssid,
                    status.connected ? status.ip_address : NULL);
                (void)ui_diagnostic_update(&network_state);
            }
            (void)snprintf(last_ip, sizeof(last_ip), "%s", status.ip_address);
            if (!last_connected && status.connected) {
                ESP_LOGI(TAG, "Wi-Fi connected; local address %s",
                         status.ip_address);
            }
            if (last_connected && !status.connected) {
                disconnected_since = xTaskGetTickCount();
            }
            last_connected = status.connected;
            (void)adsb_client_set_online(status.connected);
            if (status.connected) {
                start_mdns();
            }
        }
        if (status.connected &&
            interval_elapsed(last_rssi_refresh, RSSI_REFRESH_INTERVAL_MS)) {
            last_rssi_refresh = xTaskGetTickCount();
            (void)connectivity_refresh_rssi();
        }

        airtrack_snapshot_t aircraft;
        ESP_ERROR_CHECK(adsb_client_get_snapshot(&aircraft));
        const bool snapshot_changed = aircraft.sequence != last_snapshot_sequence;
        set_status_led(&led, night && settings.night_led_off ? LED_OFF
                             : status.connected &&
                                     (aircraft.state == AIRTRACK_FEED_LIVE ||
                                      aircraft.state == AIRTRACK_FEED_EMPTY)
                                 ? LED_BLUE : LED_ORANGE);
        if (snapshot_changed) {
            last_snapshot_sequence = aircraft.sequence;
            const esp_err_t log_result = storage_logger_submit(&aircraft);
            if (log_result != ESP_OK && log_result != ESP_ERR_NO_MEM &&
                log_result != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Could not queue SD log record: %s",
                         esp_err_to_name(log_result));
            }
        }

        if ((status.connected || s_tracking_started) &&
            (snapshot_changed || interval_elapsed(last_ui_update,
                                                  UI_TRACKING_INTERVAL_MS))) {
            last_ui_update = xTaskGetTickCount();
            const ui_tracking_state_t tracking_state = {
                .settings = &settings,
                .snapshot = &aircraft,
                .ssid = status.ssid[0] != '\0' ? status.ssid : s_config.wifi_ssid,
                .ip_address = status.connected ? status.ip_address : "--",
                .rssi_available = status.rssi_available,
                .rssi_dbm = status.rssi_dbm,
                .wifi_connected = status.connected,
            };
            const esp_err_t ui_result =
                ui_diagnostic_show_tracking(&tracking_state);
            if (ui_result != ESP_OK) {
                ESP_LOGW(TAG, "Could not update tracking display: %s",
                         esp_err_to_name(ui_result));
            } else {
                s_tracking_started = true;
            }
        }

        if (!status.connected) {
            const esp_err_t web_result = stop_status_web_if_running();
            if (web_result != ESP_OK) {
                ESP_LOGW(TAG, "Could not stop LAN status server: %s",
                         esp_err_to_name(web_result));
            }
        } else {
            const status_web_snapshot_t web_snapshot =
                make_status_web_snapshot(&status, &aircraft, &settings);
            if (status_web_is_running()) {
                if (status_changed || snapshot_changed ||
                    interval_elapsed(last_web_update, UI_TRACKING_INTERVAL_MS)) {
                    last_web_update = xTaskGetTickCount();
                    const esp_err_t web_result =
                        status_web_update(&web_snapshot);
                    if (web_result != ESP_OK) {
                        ESP_LOGW(TAG, "Could not update LAN status: %s",
                                 esp_err_to_name(web_result));
                    }
                }
            } else if (interval_elapsed(last_status_web_attempt,
                                        STATUS_WEB_RETRY_MS)) {
                last_status_web_attempt = xTaskGetTickCount();
                const esp_err_t web_result = status_web_start(
                    &web_snapshot, save_dashboard_settings,
                    reboot_from_dashboard, factory_reset_from_dashboard, NULL);
                if (web_result != ESP_OK) {
                    ESP_LOGW(TAG, "Could not start LAN status server: %s",
                             esp_err_to_name(web_result));
                } else {
                    ESP_LOGI(TAG, "Supervisor stack minimum free: %u bytes",
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                }
            }
        }

        if (boot_hold_triggered(&boot_hold)) {
            ESP_LOGI(TAG, "BOOT hold requested secure setup mode");
            return SETUP_REASON_BUTTON;
        }

        if (!status.connected &&
            interval_elapsed(disconnected_since, WIFI_RECOVERY_DELAY_MS)) {
            ESP_LOGW(TAG, "Station unavailable for %u s; opening recovery setup",
                     (unsigned)(WIFI_RECOVERY_DELAY_MS / 1000U));
            return SETUP_REASON_RECOVERY;
        }
#ifdef AIRTRACK_TEST_FORCE_RECOVERY_MS
        if (!forced_once &&
            interval_elapsed(entered, AIRTRACK_TEST_FORCE_RECOVERY_MS)) {
            forced_once = true;
            ESP_LOGW(TAG, "TEST HOOK: forcing recovery setup mode");
            return SETUP_REASON_RECOVERY;
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_INTERVAL_MS));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(airtrack_config_init());
    ESP_ERROR_CHECK(airtrack_config_load(&s_config));
    ESP_ERROR_CHECK(airtrack_config_load_settings(&s_settings));
    apply_timezone(&s_settings);

    board_config_t board_config = BOARD_CONFIG_DEFAULT();
    board_config.startup_brightness_percent = 0;

    ESP_ERROR_CHECK(board_init(&board_config));
    ESP_ERROR_CHECK(board_get_status(&s_board_status));
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &s_flash_bytes));

    ESP_LOGI(TAG, "AirTrack hardware bring-up");
    ESP_LOGI(TAG, "Flash: %lu MiB",
             (unsigned long)(s_flash_bytes / (1024U * 1024U)));
    ESP_LOGI(TAG, "LCD: %s", s_board_status.lcd_ready ? "ready" : "failed");
    ESP_LOGI(TAG, "SD: %s",
             s_board_status.sd_mounted ? "mounted" : "unavailable");

    ESP_ERROR_CHECK(ui_diagnostic_init(board_lcd_panel_io(), board_lcd_panel()));
    const ui_diagnostic_state_t diagnostic = make_diagnostic(
        s_board_status.sd_mounted ? "Starting network" : "SD optional - network",
        s_config.wifi_configured ? s_config.wifi_ssid : NULL, NULL);
    ESP_ERROR_CHECK(ui_diagnostic_update(&diagnostic));

    /* Let the first bounded LVGL strips reach the panel before illuminating it. */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(board_backlight_set(s_settings.brightness_percent));
    set_rgb(12, 4, 0);

    ESP_ERROR_CHECK(connectivity_init());
    ESP_ERROR_CHECK(adsb_client_start(&s_settings));

    if (!s_config.wifi_configured) {
        ESP_ERROR_CHECK(enter_setup_mode(SETUP_REASON_UNCONFIGURED));
        run_setup_mode(SETUP_REASON_UNCONFIGURED);
        return; /* unreachable: run_setup_mode restarts */
    }

    start_time_sync();
    ESP_ERROR_CHECK(storage_logger_start(s_board_status.sd_mounted,
                                         &s_settings));
    ESP_ERROR_CHECK(connectivity_start_station(s_config.wifi_ssid,
                                               s_config.wifi_password));

    for (;;) {
        const setup_reason_t reason = run_tracking_mode();
        ESP_ERROR_CHECK(enter_setup_mode(reason));
        run_setup_mode(reason);
        /* Only recovery returns here, with the station connected again. */
        const esp_err_t err = leave_setup_mode();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Setup teardown reported %s; restarting for a clean state",
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
}
