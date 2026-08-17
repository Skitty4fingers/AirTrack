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

# Bench-only hook: AIRTRACK_TEST_FORCE_RECOVERY_MS=<ms> in the environment
# makes the supervisor enter recovery setup once after that many milliseconds
# of tracking, so the AP+STA recovery round trip can be verified with the
# router still present.  Never set for release builds.
if(DEFINED ENV{AIRTRACK_TEST_FORCE_RECOVERY_MS})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        AIRTRACK_TEST_FORCE_RECOVERY_MS=$ENV{AIRTRACK_TEST_FORCE_RECOVERY_MS})
    message(WARNING "AirTrack test hook enabled: forced recovery after "
        "$ENV{AIRTRACK_TEST_FORCE_RECOVERY_MS} ms")
endif()
