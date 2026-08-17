idf_component_register(
    SRCS "airtrack_config.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_hw_support
        nvs_flash
)
