set(board_srcs
    "board.c"
    "board_io.c"
    "board_lcd.c"
    "board_sd.c")

# Public requirements: board.h exposes the shared I2C bus handle on boards
# that have one, so the i2c driver has to be visible to every consumer.
set(board_requires esp_lcd freertos sdmmc)
set(board_priv_requires
    esp_driver_gpio
    esp_driver_spi
    fatfs
    heap
    log)

if(CONFIG_AIRTRACK_BOARD_TOUCH_LCD_2_8)
    # Panel reset and backlight live behind the CH32V003 expander.
    list(APPEND board_srcs "board_exio.c")
    list(APPEND board_requires esp_driver_i2c)
    # usb_serial_jtag_is_connected() tells the battery code whether a USB
    # host is attached, which is what distinguishes charging from running
    # down the cell.
    list(APPEND board_priv_requires esp_driver_usb_serial_jtag)
else()
    # Direct LEDC backlight pin and a WS2812 status LED.
    list(APPEND board_priv_requires esp_driver_ledc espressif__led_strip)
endif()

idf_component_register(
    SRCS ${board_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES ${board_requires}
    PRIV_REQUIRES ${board_priv_requires}
)
