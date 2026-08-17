#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Waveshare ESP32-C6-LCD-1.47 fixed hardware geometry. */
#define BOARD_LCD_H_RES 172U
#define BOARD_LCD_V_RES 320U
#define BOARD_LCD_X_GAP 34U
#define BOARD_LCD_Y_GAP 0U
#define BOARD_LCD_STRIP_LINES 20U
#define BOARD_LCD_STRIP_BYTES (BOARD_LCD_H_RES * BOARD_LCD_STRIP_LINES * sizeof(uint16_t))

#define BOARD_SPI_MAX_TRANSFER_BYTES (8U * 1024U)
#define BOARD_SD_MOUNT_POINT "/sd"
#define BOARD_BACKLIGHT_MAX_PERCENT 50U

/*
 * Waveshare's native ESP-IDF demo provides {0xA4, 0xA1} for command D0 but
 * transmits only the first byte. The Arduino demo transmits both. Production
 * defaults to the native behavior; set board_config_t::st7789_d0_param_count
 * to the Arduino value only for the planned on-hardware A/B test.
 */
#define BOARD_ST7789_D0_NATIVE_PARAM_COUNT 1U
#define BOARD_ST7789_D0_ARDUINO_PARAM_COUNT 2U
#define BOARD_ST7789_D0_DEFAULT_PARAM_COUNT BOARD_ST7789_D0_NATIVE_PARAM_COUNT

typedef struct {
    bool mount_sd;
    bool init_rgb;
    uint8_t startup_brightness_percent;
    uint8_t st7789_d0_param_count;
} board_config_t;

#define BOARD_CONFIG_DEFAULT()                                      \
    {                                                               \
        .mount_sd = true,                                           \
        .init_rgb = true,                                           \
        .startup_brightness_percent = 45U,                          \
        .st7789_d0_param_count = BOARD_ST7789_D0_DEFAULT_PARAM_COUNT, \
    }

typedef struct {
    bool initialized;
    bool spi_ready;
    bool lcd_ready;
    bool sd_mount_attempted;
    bool sd_mounted;
    bool rgb_init_attempted;
    bool rgb_ready;
    bool button_ready;
    esp_err_t sd_mount_result;
    esp_err_t rgb_init_result;
    uint64_t sd_capacity_bytes;
    uint8_t brightness_percent;
    uint8_t st7789_d0_param_count;
} board_status_t;

/**
 * Initialize the fixed Waveshare board hardware.
 *
 * Initialization is deliberately single-shot: safe GPIO levels, LEDC at zero
 * duty, SPI2, optional non-formatting SD mount, then LCD. The LCD is cleared in
 * bounded strips before the requested (clamped) startup brightness is applied.
 * A missing or unreadable SD card and an unavailable RGB LED are non-fatal and
 * are reported through board_get_status().
 */
esp_err_t board_init(const board_config_t *config);

/** Deinitialize board-owned peripherals. Call only after UI/storage tasks stop. */
esp_err_t board_deinit(void);

bool board_is_initialized(void);
esp_err_t board_get_status(board_status_t *status);

/** Panel handles remain owned by the board component. */
esp_lcd_panel_handle_t board_lcd_panel(void);
esp_lcd_panel_io_handle_t board_lcd_panel_io(void);

/**
 * Register or replace the asynchronous color-transfer completion callback.
 * Passing NULL as callback unregisters it. The callback executes in ISR
 * context and must only notify the waiting UI task.
 */
esp_err_t board_lcd_register_color_done_callback(
    esp_lcd_panel_io_color_trans_done_cb_t callback,
    void *user_ctx);

/**
 * Borrow the shared SPI2 bus for one logical operation.
 *
 * For an asynchronous display flush, the UI task must hold this gate from
 * before esp_lcd_panel_draw_bitmap() until its completion notification is
 * received. The same UI task then releases the gate. ISR callbacks must never
 * release it. Storage must hold the gate around every bounded FATFS operation.
 */
bool board_spi_acquire(TickType_t timeout_ticks);
void board_spi_release(void);

/** Set active-high LCD backlight PWM. Every call is clamped to 0..50 percent. */
esp_err_t board_backlight_set(uint8_t percent);
uint8_t board_backlight_get(void);

/** GPIO9 is active low. Debouncing and the five-second hold policy live above BSP. */
bool board_boot_button_is_pressed(void);

/** Optional GPIO8 WS2812B support, initialized only after application startup. */
esp_err_t board_rgb_init(void);
esp_err_t board_rgb_set(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t board_rgb_clear(void);

bool board_sd_is_mounted(void);
const sdmmc_card_t *board_sd_card(void);
const char *board_sd_mount_point(void);

#ifdef __cplusplus
}
#endif
