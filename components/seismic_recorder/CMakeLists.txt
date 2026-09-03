idf_component_register(
    SRCS
        "seismic_recorder.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        object_store
        seismic_acquisition
        seismic_buffer
        storage_manager
        freertos
        json
        log
        esp_timer
)
