idf_component_register(
    SRCS
        "setup_web.c"
        "status_web.c"
    INCLUDE_DIRS "include"
    REQUIRES
        config
        esp_http_server
        tracker
    PRIV_REQUIRES
        esp_app_format
        esp_hw_support
        freertos
        log
)
