idf_component_register(
    SRCS "storage_logger.c"
    INCLUDE_DIRS "include"
    REQUIRES board config tracker
    PRIV_REQUIRES fatfs freertos log
)

