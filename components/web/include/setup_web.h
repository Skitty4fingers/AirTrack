#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airtrack_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETUP_WEB_SSID_MAX_BYTES 32U
#define SETUP_WEB_PASSWORD_MAX_BYTES 63U
#define SETUP_WEB_IPV4_TEXT_MAX_BYTES 15U
#define SETUP_WEB_MAX_NETWORKS 16U

typedef struct {
    char ssid[SETUP_WEB_SSID_MAX_BYTES + 1U];
    int8_t rssi;
    bool secured;
} setup_web_network_t;

/**
 * A validated setup-form submission.
 *
 * Wi-Fi fields are always present.  Every tracker/display value starts from
 * the settings supplied in setup_web_config_t::current_settings and is
 * overwritten only by the fields the browser actually posted, so an older
 * form or a partially filled advanced section never resets other settings.
 */
typedef struct {
    char ssid[SETUP_WEB_SSID_MAX_BYTES + 1U];
    char password[SETUP_WEB_PASSWORD_MAX_BYTES + 1U];
    airtrack_settings_t settings;
} setup_web_submission_t;

typedef esp_err_t (*setup_web_save_config_cb_t)(
    const setup_web_submission_t *submission, void *user_context);

typedef struct {
    /* Values shown to the user for joining the device's setup hotspot. */
    const char *ap_ssid;
    const char *ap_password;
    const char *ap_ip_address;

    /* Nearby networks are copied when the server starts and rendered in the
     * order supplied. Hidden networks can still be entered manually. */
    const setup_web_network_t *nearby_networks;
    size_t nearby_network_count;

    /* Optional: the currently saved station SSID, pre-filled in the form.
     * The station password is never supplied or shown. */
    const char *current_ssid;

    /* Required: the effective tracker settings.  Location, radius, units,
     * brightness, poll interval, ground filter, and logging mode are
     * pre-filled from here and used as the base for the submission. */
    const airtrack_settings_t *current_settings;

    /* Called after POST input is decoded and validated. */
    setup_web_save_config_cb_t save_config;
    void *user_context;
} setup_web_config_t;

/** Start the local setup server. Configuration strings and networks are copied. */
esp_err_t setup_web_start(const setup_web_config_t *config);

/** Stop the server and release its sockets. */
esp_err_t setup_web_stop(void);

bool setup_web_is_running(void);

#ifdef __cplusplus
}
#endif
