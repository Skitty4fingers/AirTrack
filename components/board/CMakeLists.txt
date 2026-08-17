idf_component_register(
    SRCS
        "board.c"
        "board_io.c"
        "board_lcd.c"
        "board_sd.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        freertos
        sdmmc
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        espressif__led_strip
        fatfs
        heap
        log
)
