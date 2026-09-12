#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "board.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional on-board sensors sharing the board's I2C bus.
 *
 * Only the ESP32-C6-Touch-LCD-2.8 carries them.  On every other board these
 * calls compile to stubs returning ESP_ERR_NOT_SUPPORTED with a status that
 * reports nothing present, so callers need no conditional code.
 *
 * Both devices are optional at runtime as well: a board whose sensors do not
 * answer keeps working exactly as one without them.
 */

typedef struct {
    /* Built for a board that has these sensors at all. */
    bool supported;
    /* PCF85063A answered at start-up. */
    bool rtc_present;
    /* The RTC reported a running oscillator and a plausible date. */
    bool rtc_time_valid;
    /* SHTC3 answered its ID query at start-up. */
    bool environment_present;
    /* Newest cached sample; only meaningful with environment_valid. */
    bool environment_valid;
    float temperature_c;
    float humidity_percent;
} board_sensors_status_t;

/**
 * Probe the optional sensors on the shared I2C bus.
 *
 * Call after board_init().  Never fails the caller: a missing or unresponsive
 * device is recorded in the status and reported through the dashboard.
 */
esp_err_t board_sensors_init(void);

/** Copy the current sensor status, including the newest cached sample. */
esp_err_t board_sensors_get_status(board_sensors_status_t *status);

/**
 * Read the battery-backed clock.
 *
 * Returns ESP_ERR_INVALID_STATE when no RTC is present, and ESP_ERR_NOT_FOUND
 * when the chip reports that its oscillator stopped, which is what a board
 * with a flat or absent backup cell reports on a cold start.
 */
esp_err_t board_sensors_rtc_get(time_t *utc);

/** Write UTC to the battery-backed clock, clearing its oscillator-stop flag. */
esp_err_t board_sensors_rtc_set(time_t utc);

/**
 * Sample temperature and humidity, no more often than every few seconds.
 *
 * Repeated calls inside the cache window return the previous sample without
 * touching the bus, so the dashboard's two-second refresh is free.  Either
 * pointer may be NULL.
 */
esp_err_t board_sensors_read_environment(float *temperature_c,
                                         float *humidity_percent);

#ifdef __cplusplus
}
#endif
