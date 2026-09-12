#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

#if defined(CONFIG_AIRTRACK_BOARD_TOUCH_LCD_2_8)
#include "driver/i2c_master.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fixed hardware geometry and capabilities of the selected board.
 *
 * BOARD_ID is the stable identifier used by the OTA manifest so a device
 * refuses an image built for a different board.  Both supported panels are
 * 320 pixels tall, so only the width and the RAM offset vary; the UI lays
 * itself out from BOARD_LCD_H_RES rather than from literal coordinates.
 */
#if defined(CONFIG_AIRTRACK_BOARD_LCD_1_47)

#define BOARD_ID "esp32c6-lcd-1.47"
#define BOARD_NAME "Waveshare ESP32-C6-LCD-1.47"
#define BOARD_LCD_H_RES 172U
#define BOARD_LCD_V_RES 320U
#define BOARD_LCD_X_GAP 34U
#define BOARD_LCD_Y_GAP 0U
/* One WS2812B on GPIO8 mirrors the feed state. */
#define BOARD_HAS_RGB_LED 1
/* Backlight is a direct LEDC PWM pin. */
#define BOARD_HAS_I2C_BUS 0
#define BOARD_HAS_IO_EXPANDER 0
#define BOARD_HAS_RTC 0
#define BOARD_HAS_ENVIRONMENT_SENSOR 0
#define BOARD_HAS_BATTERY_SENSE 0
/* One 172 x 20 RGB565 strip is 6,880 bytes. */
#define BOARD_SPI_MAX_TRANSFER_BYTES (8U * 1024U)

#elif defined(CONFIG_AIRTRACK_BOARD_TOUCH_LCD_2_8)

#define BOARD_ID "esp32c6-touch-lcd-2.8"
#define BOARD_NAME "Waveshare ESP32-C6-Touch-LCD-2.8"
#define BOARD_LCD_H_RES 240U
#define BOARD_LCD_V_RES 320U
#define BOARD_LCD_X_GAP 0U
#define BOARD_LCD_Y_GAP 0U
/* No status LED on this board; LED calls report ESP_ERR_NOT_SUPPORTED. */
#define BOARD_HAS_RGB_LED 0
/* Shared 400 kHz bus: CH32 expander, PCF85063A, SHTC3, QMI8658, ES8311. */
#define BOARD_HAS_I2C_BUS 1
/* Panel reset and backlight PWM are behind the CH32V003 expander at 0x24. */
#define BOARD_HAS_IO_EXPANDER 1
#define BOARD_HAS_RTC 1
#define BOARD_HAS_ENVIRONMENT_SENSOR 1
/* Battery voltage reaches the expander's ADC as BAT_ADC. */
#define BOARD_HAS_BATTERY_SENSE 1
/* One 240 x 20 RGB565 strip is 9,600 bytes and needs the larger budget. */
#define BOARD_SPI_MAX_TRANSFER_BYTES (16U * 1024U)

#else
#error "No AirTrack board selected; run idf.py menuconfig and pick one"
#endif

#define BOARD_LCD_STRIP_LINES 20U
#define BOARD_LCD_STRIP_BYTES (BOARD_LCD_H_RES * BOARD_LCD_STRIP_LINES * sizeof(uint16_t))

#define BOARD_SD_MOUNT_POINT "/sd"
#define BOARD_BACKLIGHT_MAX_PERCENT 50U

/*
 * Waveshare's native ESP-IDF demo for the 1.47 provides {0xA4, 0xA1} for
 * command D0 but transmits only the first byte. The Arduino demo transmits
 * both. That board defaults to the native behavior; set
 * board_config_t::st7789_d0_param_count to the Arduino value only for the
 * planned on-hardware A/B test.
 *
 * The 2.8 ships only an Arduino demo, which transmits both bytes, so that is
 * the default there and the A/B knob exists for the same reason in reverse.
 */
#define BOARD_ST7789_D0_NATIVE_PARAM_COUNT 1U
#define BOARD_ST7789_D0_ARDUINO_PARAM_COUNT 2U
#if BOARD_HAS_IO_EXPANDER
#define BOARD_ST7789_D0_DEFAULT_PARAM_COUNT BOARD_ST7789_D0_ARDUINO_PARAM_COUNT
#else
#define BOARD_ST7789_D0_DEFAULT_PARAM_COUNT BOARD_ST7789_D0_NATIVE_PARAM_COUNT
#endif

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

/**
 * Set the LCD backlight level. Every call is clamped to 0..50 percent.
 *
 * On boards with a direct backlight pin this is an active-high LEDC PWM.
 * Where the backlight is behind the I/O expander the same call becomes a
 * short I2C write, so it must not be issued from an ISR.
 */
esp_err_t board_backlight_set(uint8_t percent);
uint8_t board_backlight_get(void);

/** GPIO9 is active low. Debouncing and the five-second hold policy live above BSP. */
bool board_boot_button_is_pressed(void);

/**
 * Optional single WS2812B status LED, initialized only after application
 * startup because its pin is also a boot strap.
 *
 * Boards without an LED (BOARD_HAS_RGB_LED == 0) return
 * ESP_ERR_NOT_SUPPORTED from every call and report rgb_ready false; callers
 * already treat the LED as best-effort.
 */
esp_err_t board_rgb_init(void);
esp_err_t board_rgb_set(uint8_t red, uint8_t green, uint8_t blue);
esp_err_t board_rgb_clear(void);

#if BOARD_HAS_BATTERY_SENSE
/**
 * Read the raw battery sense channel and the expander's input register.
 *
 * Waveshare document the expander's ADC as BAT_ADC but publish neither the
 * divider ratio nor a charge-status signal, so both are established by
 * measurement on real hardware.  This accessor exposes the unscaled values so
 * that calibration is possible; higher layers should use the cooked reading.
 *
 * Either pointer may be NULL.
 */
esp_err_t board_battery_raw(uint16_t *adc_counts, uint8_t *expander_inputs);
#endif

#if BOARD_HAS_I2C_BUS
/**
 * Shared I2C master bus, owned by the board component.
 *
 * NULL until board_init() succeeds, and NULL if the bus failed to come up (a
 * board with an I/O expander cannot reach that state, since the panel needs
 * the bus).  On-board sensors attach their own devices to this handle; the
 * bus driver serializes concurrent transactions internally.
 */
i2c_master_bus_handle_t board_i2c_bus(void);
#endif

bool board_sd_is_mounted(void);
const sdmmc_card_t *board_sd_card(void);
const char *board_sd_mount_point(void);

#ifdef __cplusplus
}
#endif
