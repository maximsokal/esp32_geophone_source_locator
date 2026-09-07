idf_component_register(
    SRCS
        "seismic_recorder.c"
        "seismic_recorder_delete.c"
        "seismic_recorder_fs.c"
        "seismic_recorder_json.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

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
