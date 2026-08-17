#include "airtrack_config.h"
#include "adsb_client.h"
#include "board.h"
#include "captive_dns.h"
#include "connectivity.h"
#include "setup_web.h"
#include "status_web.h"
#include "storage_logger.h"

#include <stdio.h>
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
#define NETWORK_STATUS_INTERVAL_MS 500U
#define SETTINGS_RESTART_DELAY_MS 2500U
#define SETTINGS_RESTART_STACK_BYTES 2048U
#define STATUS_WEB_RETRY_MS 5000U
#define UI_TRACKING_INTERVAL_MS 1000U
#define BOOT_SETUP_HOLD_MS 5000U
#define VALID_TIME_EPOCH 1704067200L

static const char *TAG = "airtrack";
static portMUX_TYPE s_restart_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_restart_scheduled;
static StaticTask_t s_restart_task_storage;
static StackType_t s_restart_task_stack[
    SETTINGS_RESTART_STACK_BYTES / sizeof(StackType_t)];

static board_status_t s_board_status;
static uint32_t s_flash_bytes;
static airtrack_settings_t s_settings;
static bool s_mdns_started;
static bool s_sntp_started;
static connectivity_network_t
    s_scanned_networks[CONNECTIVITY_SCAN_MAX_RESULTS];
static setup_web_network_t s_web_networks[SETUP_WEB_MAX_NETWORKS];

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

static esp_err_t save_device_settings(const char *ssid, const char *password,
                                      int32_t latitude_e7,
                                      int32_t longitude_e7,
                                      uint16_t radius_nm,
                                      void *user_context)
{
    (void)user_context;

    if (!claim_settings_restart()) {
        return ESP_ERR_INVALID_STATE;
    }

    airtrack_settings_t updated = s_settings;
    updated.location_configured = true;
    updated.latitude_e7 = latitude_e7;
    updated.longitude_e7 = longitude_e7;
    updated.radius_nm = radius_nm;
    esp_err_t err = airtrack_settings_validate(&updated);
    if (err == ESP_OK) {
        err = airtrack_config_save_settings(&updated);
    }
    if (err == ESP_OK) {
        err = airtrack_config_save_wifi(ssid, password);
    }
    if (err == ESP_OK) {
        err = launch_settings_restart();
    }

    if (err != ESP_OK) {
        release_settings_restart();
        return err;
    }

    ESP_LOGI(TAG, "Wi-Fi settings saved; restart scheduled");
    return ESP_OK;
}

static esp_err_t save_initial_location(int32_t latitude_e7,
                                       int32_t longitude_e7,
                                       uint16_t radius_nm,
                                       void *user_context)
{
    (void)user_context;
    if (s_settings.location_configured || !claim_settings_restart()) {
        return ESP_ERR_INVALID_STATE;
    }

    airtrack_settings_t updated = s_settings;
    updated.location_configured = true;
    updated.latitude_e7 = latitude_e7;
    updated.longitude_e7 = longitude_e7;
    updated.radius_nm = radius_nm;
    esp_err_t err = airtrack_settings_validate(&updated);
    if (err == ESP_OK) {
        err = airtrack_config_save_settings(&updated);
    }
    if (err == ESP_OK) {
        err = launch_settings_restart();
    }
    if (err != ESP_OK) {
        release_settings_restart();
        return err;
    }

    ESP_LOGI(TAG, "Initial tracking location saved; restart scheduled");
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
    const airtrack_snapshot_t *aircraft)
{
    return (status_web_snapshot_t) {
        .ssid = status->ssid,
        .ip_address = status->ip_address,
        .rssi_available = status->rssi_available,
        .rssi_dbm = status->rssi_dbm,
        .sd_mounted = s_board_status.sd_mounted,
        .flash_bytes = s_flash_bytes,
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
        .free_heap_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT),
        .minimum_free_heap_bytes = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
        .time_synchronized = time(NULL) >= VALID_TIME_EPOCH,
        .settings = &s_settings,
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

static esp_err_t enter_setup_mode(const airtrack_config_t *config)
{
    esp_err_t err = stop_status_web_if_running();
    if (err != ESP_OK) {
        return err;
    }

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
        err = connectivity_stop_station();
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGI(TAG, "Station paused for isolated setup mode");
    }

    const size_t nearby_network_count = scan_nearby_networks();

    err = connectivity_start_softap(config->ap_ssid, config->ap_password);
    if (err != ESP_OK) {
        return err;
    }

    const setup_web_config_t web_config = {
        .ap_ssid = config->ap_ssid,
        .ap_password = config->ap_password,
        .ap_ip_address = CONNECTIVITY_SOFTAP_IPV4,
        .nearby_networks = s_web_networks,
        .nearby_network_count = nearby_network_count,
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

    err = ui_diagnostic_show_setup(config->ap_ssid, config->ap_password,
                                   CONNECTIVITY_SOFTAP_IPV4);
    if (err != ESP_OK) {
        return err;
    }

    if (s_board_status.rgb_ready) {
        (void)board_rgb_set(8, 3, 0);
    }
    ESP_LOGI(TAG, "Setup mode ready on %s", config->ap_ssid);
    return ESP_OK;
}

static bool interval_elapsed(TickType_t started, uint32_t interval_ms)
{
    return (TickType_t)(xTaskGetTickCount() - started) >=
           pdMS_TO_TICKS(interval_ms);
}

void app_main(void)
{
    ESP_ERROR_CHECK(airtrack_config_init());
    airtrack_config_t config;
    ESP_ERROR_CHECK(airtrack_config_load(&config));
    ESP_ERROR_CHECK(airtrack_config_load_settings(&s_settings));

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
        config.wifi_configured ? config.wifi_ssid : NULL, NULL);
    ESP_ERROR_CHECK(ui_diagnostic_update(&diagnostic));

    /* Let the first bounded LVGL strips reach the panel before illuminating it. */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(board_backlight_set(s_settings.brightness_percent));

    if (s_board_status.rgb_ready) {
        ESP_ERROR_CHECK(board_rgb_set(8, 3, 0));
    }

    ESP_ERROR_CHECK(connectivity_init());
    if (!config.wifi_configured) {
        ESP_ERROR_CHECK(enter_setup_mode(&config));
        return;
    }

    start_time_sync();
    ESP_ERROR_CHECK(adsb_client_start(&s_settings));
    ESP_ERROR_CHECK(storage_logger_start(s_board_status.sd_mounted,
                                         &s_settings));
    ESP_ERROR_CHECK(connectivity_start_station(config.wifi_ssid,
                                               config.wifi_password));
    TickType_t disconnected_since = xTaskGetTickCount();
    TickType_t last_status_web_attempt =
        xTaskGetTickCount() - pdMS_TO_TICKS(STATUS_WEB_RETRY_MS);
    bool last_connected = false;
    char last_ip[CONNECTIVITY_IPV4_TEXT_MAX_BYTES + 1U] = {0};
    uint64_t last_snapshot_sequence = UINT64_MAX;
    TickType_t last_ui_update = 0U;
    TickType_t last_web_update = 0U;
    TickType_t boot_pressed_since = 0U;
    bool boot_hold_active = false;
    bool tracking_screen_started = false;

    for (;;) {
        connectivity_status_t status;
        ESP_ERROR_CHECK(connectivity_get_status(&status));

        const bool status_changed = status.connected != last_connected ||
                                    strcmp(status.ip_address, last_ip) != 0;
        if (status_changed) {
            if (!tracking_screen_started) {
                const ui_diagnostic_state_t network_state = make_diagnostic(
                    status.connected ? "Wi-Fi connected" : "Connecting to Wi-Fi",
                    status.ssid[0] != '\0' ? status.ssid : config.wifi_ssid,
                    status.connected ? status.ip_address : NULL);
                ESP_ERROR_CHECK(ui_diagnostic_update(&network_state));
            }
            (void)snprintf(last_ip, sizeof(last_ip), "%s", status.ip_address);

            if (s_board_status.rgb_ready) {
                (void)board_rgb_set(status.connected ? 0 : 8,
                                    status.connected ? 8 : 3,
                                    status.connected ? 2 : 0);
            }
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

        airtrack_snapshot_t aircraft;
        ESP_ERROR_CHECK(adsb_client_get_snapshot(&aircraft));
        const bool snapshot_changed = aircraft.sequence != last_snapshot_sequence;
        if (snapshot_changed) {
            last_snapshot_sequence = aircraft.sequence;
            const esp_err_t log_result = storage_logger_submit(&aircraft);
            if (log_result != ESP_OK && log_result != ESP_ERR_NO_MEM) {
                ESP_LOGW(TAG, "Could not queue SD log record: %s",
                         esp_err_to_name(log_result));
            }
        }

        if ((status.connected || tracking_screen_started) &&
            (snapshot_changed || interval_elapsed(last_ui_update,
                                                  UI_TRACKING_INTERVAL_MS))) {
            last_ui_update = xTaskGetTickCount();
            const ui_tracking_state_t tracking_state = {
                .settings = &s_settings,
                .snapshot = &aircraft,
                .ssid = status.ssid[0] != '\0' ? status.ssid : config.wifi_ssid,
                .ip_address = status.connected ? status.ip_address : "--",
                .rssi_available = status.rssi_available,
                .rssi_dbm = status.rssi_dbm,
            };
            const esp_err_t ui_result =
                ui_diagnostic_show_tracking(&tracking_state);
            if (ui_result != ESP_OK) {
                ESP_LOGW(TAG, "Could not update tracking display: %s",
                         esp_err_to_name(ui_result));
            } else {
                tracking_screen_started = true;
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
                make_status_web_snapshot(&status, &aircraft);
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
                    &web_snapshot, save_initial_location, NULL);
                if (web_result != ESP_OK) {
                    ESP_LOGW(TAG, "Could not start LAN status server: %s",
                             esp_err_to_name(web_result));
                } else {
                    ESP_LOGI(TAG, "Supervisor stack minimum free: %u bytes",
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                }
            }
        }


        if (board_boot_button_is_pressed()) {
            if (!boot_hold_active) {
                boot_hold_active = true;
                boot_pressed_since = xTaskGetTickCount();
            } else if (interval_elapsed(boot_pressed_since,
                                       BOOT_SETUP_HOLD_MS)) {
                ESP_LOGI(TAG, "BOOT hold requested secure setup mode");
                (void)adsb_client_set_online(false);
                (void)storage_logger_stop();
                ESP_ERROR_CHECK(enter_setup_mode(&config));
                return;
            }
        } else {
            boot_hold_active = false;
        }

        if (!status.connected &&
            interval_elapsed(disconnected_since, WIFI_RECOVERY_DELAY_MS)) {
            ESP_LOGW(TAG, "Station unavailable; enabling recovery setup mode");
            (void)storage_logger_stop();
            ESP_ERROR_CHECK(enter_setup_mode(&config));
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(NETWORK_STATUS_INTERVAL_MS));
    }
}
