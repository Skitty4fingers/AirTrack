idf_component_register(
    SRCS "adsb_client.c"
    INCLUDE_DIRS "include"
    REQUIRES config tracker
    PRIV_REQUIRES esp_http_client esp_timer freertos log mbedtls
)

