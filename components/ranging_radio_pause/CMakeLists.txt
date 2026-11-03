idf_component_register(
    SRCS
        "ranging_radio_pause_core.c"
        "ranging_radio_pause.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_timer
        freertos
        log
        wifi_manager
)
