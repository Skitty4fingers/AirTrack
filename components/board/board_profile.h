/*
 * Per-board hardware constants.
 *
 * Everything that differs between supported boards lives here so the rest of
 * the board component reads the same on either target.  Public geometry and
 * capability macros are in board.h; this header holds the pin map and the
 * panel details that only the board component needs.
 */
#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_AIRTRACK_BOARD_LCD_1_47)

/* Waveshare ESP32-C6-LCD-1.47 (ESP32-C6FH8, 8 MB flash, no PSRAM). */
#define BOARD_PIN_SPI_MOSI 6
#define BOARD_PIN_SPI_MISO 5
#define BOARD_PIN_SPI_SCLK 7
#define BOARD_PIN_LCD_CS 14
#define BOARD_PIN_LCD_DC 15
#define BOARD_PIN_LCD_RESET 21
#define BOARD_PIN_BACKLIGHT 22
#define BOARD_PIN_SD_CS 4
#define BOARD_PIN_RGB 8
#define BOARD_PIN_BOOT_BUTTON 9

/* Waveshare's vendor clock for this panel; raise only after hardware soak. */
#define BOARD_LCD_PIXEL_CLOCK_HZ (12U * 1000U * 1000U)
/* The 172-pixel panel sits in a 240-pixel controller window. */
#define BOARD_LCD_MIRROR_X true
#define BOARD_LCD_MIRROR_Y false
/* This panel is wired blue-green-red. */
#define BOARD_LCD_RGB_ELEMENT_ORDER LCD_RGB_ELEMENT_ORDER_BGR

#elif defined(CONFIG_AIRTRACK_BOARD_TOUCH_LCD_2_8)

/*
 * Waveshare ESP32-C6-Touch-LCD-2.8 (ESP32-C6-WROOM-1, 16 MB flash).
 *
 * GPIO6/GPIO7 are the shared I2C bus here, not SPI as on the 1.47.  The panel
 * has no MISO line, and LCD reset plus backlight PWM are driven by the CH32
 * expander (see board_exio.c), so no ESP32 GPIO is assigned to them.
 */
#define BOARD_PIN_SPI_MOSI 1
#define BOARD_PIN_SPI_MISO 8 /* shared with the SD card; the LCD is write-only */
#define BOARD_PIN_SPI_SCLK 0
#define BOARD_PIN_LCD_CS 11
#define BOARD_PIN_LCD_DC 10
#define BOARD_PIN_LCD_RESET (-1) /* via BOARD_EXIO_LCD_RESET */
#define BOARD_PIN_BACKLIGHT (-1) /* via the expander's PWM register */
#define BOARD_PIN_SD_CS 23
#define BOARD_PIN_BOOT_BUTTON 9

#define BOARD_PIN_I2C_SDA 6
#define BOARD_PIN_I2C_SCL 7
#define BOARD_I2C_FREQUENCY_HZ 400000U

/*
 * The vendor demo clocks this panel at 80 MHz.  AirTrack shares SPI2 with the
 * SD card and only ever pushes 20-line strips, so it starts at the same
 * conservative 40 MHz the ESP-IDF ST7789 examples use for a bus with a second
 * device on it.  Raise only after a combined LCD+SD soak.
 */
#define BOARD_LCD_PIXEL_CLOCK_HZ (40U * 1000U * 1000U)
#define BOARD_LCD_MIRROR_X false
#define BOARD_LCD_MIRROR_Y false
/*
 * This panel is wired red-green-blue, matching the MADCTL 0x00 the vendor
 * sequence writes.  The order has to be declared here rather than left to the
 * init table, because esp_lcd_panel_mirror() rewrites MADCTL from the value
 * it derived from this field and would otherwise put the BGR bit back and
 * swap red with blue.
 */
#define BOARD_LCD_RGB_ELEMENT_ORDER LCD_RGB_ELEMENT_ORDER_RGB

/* CH32V003 I/O expander channel assignments (Waveshare Board_IO reference). */
#define BOARD_EXIO_TOUCH_RESET 0
#define BOARD_EXIO_LCD_RESET 1
#define BOARD_EXIO_AUDIO_ENABLE 3

#else
#error "No AirTrack board selected; run idf.py menuconfig and pick one"
#endif

#define BOARD_SPI_HOST SPI2_HOST
#define BOARD_LCD_QUEUE_DEPTH 3U
#define BOARD_SD_CLOCK_KHZ 10000
#define BOARD_SD_MAX_FILES 6
