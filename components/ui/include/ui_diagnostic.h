#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"
#include "airtrack_config.h"
#include "airtrack_tracker.h"
#include "flight_info.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_DIAGNOSTIC_PENDING = 0,
    UI_DIAGNOSTIC_OK,
    UI_DIAGNOSTIC_WARNING,
    UI_DIAGNOSTIC_ERROR,
} ui_diagnostic_result_t;

typedef struct {
    const char *phase;
    ui_diagnostic_result_t lcd;
    ui_diagnostic_result_t sd;
    ui_diagnostic_result_t flash;
    uint32_t flash_bytes;
    const char *ssid;
    const char *ip_address;
} ui_diagnostic_state_t;

/**
 * Start LVGL on the board-owned LCD and draw the hardware diagnostic screen.
 * The caller keeps ownership of both esp_lcd handles.
 */
esp_err_t ui_diagnostic_init(esp_lcd_panel_io_handle_t io,
                             esp_lcd_panel_handle_t panel);

/** Copy the supplied values into the visible diagnostic screen. */
esp_err_t ui_diagnostic_update(const ui_diagnostic_state_t *state);

/**
 * Replace the diagnostic view with the offline Wi-Fi setup screen.
 *
 * The screen contains a standards-compatible WPA Wi-Fi QR code, the readable
 * access-point credentials, and the setup web address.  When recovery is
 * true the header explains that the saved network was lost and is retried.  The SSID must contain
 * 1..32 printable bytes, the WPA password 8..63 printable bytes, and
 * ip_address must be a valid IPv4 address (normally "192.168.4.1").
 */
esp_err_t ui_diagnostic_show_setup(const char *ap_ssid,
                                   const char *ap_password,
                                   const char *ip_address,
                                   bool recovery);

typedef struct {
    const airtrack_settings_t *settings;
    const airtrack_snapshot_t *snapshot;
    const char *ssid;
    const char *ip_address;
    bool rssi_available;
    int8_t rssi_dbm;
    bool wifi_connected;
    /*
     * Optional on-board climate, shown in the footer.  Boards without the
     * sensor leave environment_valid false and the footer keeps its original
     * attribution-only line.  The unit follows settings->temperature_unit.
     */
    bool environment_valid;
    float temperature_c;
    float humidity_percent;
    /* Battery, where the board senses one.  On USB the level is withheld,
     * because the charger holds the rail near full regardless of the cell. */
    bool battery_valid;
    bool usb_present;
    uint8_t battery_percent;
    /*
     * The followed flight, while settings->focus_flight is set: its details
     * (route, schedule, phase) and an optional FLIGHT_INFO_LOGO_SIZE square
     * RGB565 airline logo.  logo_generation changes whenever the pixels do,
     * so the screen copies them only then.  Either pointer may be NULL.
     */
    const flight_info_t *flight;
    const uint16_t *logo;
    uint32_t logo_generation;
} ui_tracking_state_t;

/** Show or update the production nearest-aircraft screen. */
esp_err_t ui_diagnostic_show_tracking(const ui_tracking_state_t *state);

/**
 * Show or refresh the firmware-update screen: version being installed, a
 * progress bar (0-100), and a short phase text ("Downloading", "Verifying",
 * "Restarting", or an error).  Replaces whatever screen is visible.
 */
esp_err_t ui_diagnostic_show_updating(const char *version, uint8_t percent,
                                      const char *phase, bool failed);

esp_err_t ui_diagnostic_deinit(void);
bool ui_diagnostic_is_initialized(void);

#ifdef __cplusplus
}
#endif
