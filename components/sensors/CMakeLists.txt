set(sensors_priv_requires esp_timer freertos log)

if(CONFIG_AIRTRACK_BOARD_TOUCH_LCD_2_8)
    list(APPEND sensors_priv_requires esp_driver_i2c)
endif()

idf_component_register(
    SRCS "board_sensors.c"
    INCLUDE_DIRS "include"
    REQUIRES board
    PRIV_REQUIRES ${sensors_priv_requires}
)
