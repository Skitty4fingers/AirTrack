#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "freertos/semphr.h"

#if BOARD_HAS_RGB_LED
#include "led_strip.h"
#endif

typedef struct {
    bool init_in_progress;
    bool initialized;
    bool spi_ready;
    bool lcd_ready;
    bool backlight_ready;
    bool button_ready;
    bool sd_mount_attempted;
    bool sd_mounted;
    bool rgb_init_attempted;
    bool rgb_ready;
    esp_err_t sd_mount_result;
    esp_err_t rgb_init_result;
    uint64_t sd_capacity_bytes;
    uint8_t brightness_percent;
    uint8_t st7789_d0_param_count;
    SemaphoreHandle_t spi_gate;
    StaticSemaphore_t spi_gate_storage;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;
    sdmmc_card_t *sd_card;
#if BOARD_HAS_RGB_LED
    led_strip_handle_t rgb_strip;
#endif
#if BOARD_HAS_I2C_BUS
    i2c_master_bus_handle_t i2c_bus;
#endif
#if BOARD_HAS_IO_EXPANDER
    bool exio_ready;
    i2c_master_dev_handle_t exio_device;
    /* Shadow of the expander's output register; it is write-only in use. */
    uint8_t exio_outputs;
#endif
} board_state_t;

extern board_state_t g_board_state;

esp_err_t board_internal_prepare_safe_pins(void);
esp_err_t board_internal_backlight_init(void);
esp_err_t board_internal_backlight_set(uint8_t percent);
void board_internal_backlight_deinit(void);

esp_err_t board_internal_button_init(void);
void board_internal_button_deinit(void);

esp_err_t board_internal_rgb_init(void);
void board_internal_rgb_deinit(void);

esp_err_t board_internal_sd_mount(void);
void board_internal_sd_unmount(void);

esp_err_t board_internal_lcd_init(uint8_t d0_param_count);
void board_internal_lcd_deinit(void);

#if BOARD_HAS_I2C_BUS
/* Bring up the shared I2C bus and, where present, the I/O expander on it. */
esp_err_t board_internal_i2c_init(void);
void board_internal_i2c_deinit(void);
#endif

#if BOARD_HAS_IO_EXPANDER
/* Drive one expander output channel. */
esp_err_t board_internal_exio_set(uint8_t channel, bool level);
/* Write the expander's 8-bit backlight duty register directly. */
esp_err_t board_internal_exio_set_pwm(uint8_t duty);
/* Pulse the panel reset line with the vendor's timings. */
esp_err_t board_internal_exio_reset_panel(void);
#endif
