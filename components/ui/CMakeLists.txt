idf_component_register(
    SRCS "ui_diagnostic.c" "ui_icons.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board
        config
        esp_lcd
        espressif__esp_lvgl_port
        lvgl__lvgl
        tracker
    PRIV_REQUIRES
        espressif__qrcode
        esp_timer
        freertos
        log
)
