idf_component_register(
    SRCS "airtrack_tracker.c"
    INCLUDE_DIRS "include"
    REQUIRES config json
    PRIV_REQUIRES log
)

