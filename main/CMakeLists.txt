idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        adsb_client
        board
        captive_dns
        config
        connectivity
        storage
        ui
        web
    PRIV_REQUIRES
        espressif__mdns
        esp_timer
        heap
        lwip
        esp_system
        freertos
        log
        spi_flash
)
