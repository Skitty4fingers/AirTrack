idf_component_register(
    SRCS "connectivity.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_event
        esp_netif
        esp_wifi
        esp_timer
        freertos
    PRIV_REQUIRES
        log
)
