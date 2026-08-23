idf_component_register(
    SRCS
        "seismic_test_generator.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        seismic_buffer
        esp_timer
        freertos
        log
)