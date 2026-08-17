#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIRTRACK_AP_PASSWORD_LENGTH 8U
#define AIRTRACK_HOSTNAME_MAX_LENGTH 24U
/* Callsign, registration, or ICAO hex of a single flight to follow. */
#define AIRTRACK_FOCUS_MAX_LENGTH 8U

typedef enum {
    AIRTRACK_DISTANCE_NM = 0,
    AIRTRACK_DISTANCE_KM,
    AIRTRACK_DISTANCE_MI,
} airtrack_distance_unit_t;

typedef enum {
    AIRTRACK_LOGGING_OFF = 0,
    AIRTRACK_LOGGING_TARGET_CHANGES,
    AIRTRACK_LOGGING_PERIODIC,
} airtrack_logging_mode_t;

typedef struct {
    uint64_t generation;
    bool location_configured;
    int32_t latitude_e7;
    int32_t longitude_e7;
    uint16_t radius_nm;
    uint16_t poll_interval_s;
    uint16_t max_position_age_s;
    bool include_ground;
    airtrack_distance_unit_t distance_unit;
    uint8_t brightness_percent;
    airtrack_logging_mode_t logging_mode;
    uint16_t log_heartbeat_s;
    uint16_t retention_days;
    uint16_t retention_mib;
    char hostname[AIRTRACK_HOSTNAME_MAX_LENGTH + 1U];
    /* Empty: track the nearest aircraft.  Otherwise only this flight
     * (matched against callsign, registration, or hex, case-insensitive). */
    char focus_flight[AIRTRACK_FOCUS_MAX_LENGTH + 1U];
} airtrack_settings_t;

typedef struct {
    bool wifi_configured;
    char wifi_ssid[33];
    char wifi_password[65];
    char ap_ssid[33];
    char ap_password[AIRTRACK_AP_PASSWORD_LENGTH + 1U];
} airtrack_config_t;

/** Initialize NVS and create the device's persistent setup-network identity. */
esp_err_t airtrack_config_init(void);

/** Load the current Wi-Fi and setup-network configuration. */
esp_err_t airtrack_config_load(airtrack_config_t *out);

/** Save station credentials. Password must be empty (open) or 8..63 characters. */
esp_err_t airtrack_config_save_wifi(const char *ssid, const char *password);

/** Forget station credentials without changing the setup-network identity. */
esp_err_t airtrack_config_clear_wifi(void);

/** Fill tracker settings with conservative production defaults. */
void airtrack_settings_defaults(airtrack_settings_t *out);

/** Validate every tracker setting, including hostname and location bounds. */
esp_err_t airtrack_settings_validate(const airtrack_settings_t *settings);

/** Load the newest valid CRC-protected A/B tracker settings record. */
esp_err_t airtrack_config_load_settings(airtrack_settings_t *out);

/** Atomically persist tracker settings to the older A/B NVS slot. */
esp_err_t airtrack_config_save_settings(const airtrack_settings_t *settings);

#ifdef __cplusplus
}
#endif
