idf_component_register(
    SRCS "ota_update.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_app_format
    PRIV_REQUIRES app_update esp_http_client esp_partition esp_timer freertos json log mbedtls
)
