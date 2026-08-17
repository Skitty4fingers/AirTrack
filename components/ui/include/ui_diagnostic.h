#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"
#include "airtrack_config.h"
#include "airtrack_tracker.h"

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
 * access-point credentials, and the setup web address.  The SSID must contain
 * 1..32 printable bytes, the WPA password 8..63 printable bytes, and
 * ip_address must be a valid IPv4 address (normally "192.168.4.1").
 */
esp_err_t ui_diagnostic_show_setup(const char *ap_ssid,
                                   const char *ap_password,
                                   const char *ip_address);

typedef struct {
    const airtrack_settings_t *settings;
    const airtrack_snapshot_t *snapshot;
    const char *ssid;
    const char *ip_address;
    bool rssi_available;
    int8_t rssi_dbm;
} ui_tracking_state_t;

/** Show or update the production nearest-aircraft screen. */
esp_err_t ui_diagnostic_show_tracking(const ui_tracking_state_t *state);

esp_err_t ui_diagnostic_deinit(void);
bool ui_diagnostic_is_initialized(void);

#ifdef __cplusplus
}
#endif
