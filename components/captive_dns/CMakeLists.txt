idf_component_register(
    SRCS "captive_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        freertos
        log
        lwip
)
