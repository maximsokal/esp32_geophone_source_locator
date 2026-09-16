# Recorder lifetime ownership is interposed only at the public START boundary
# and terminal metadata call inside seismic_recorder.c. The original recorder
# state machine stays unchanged and remains available to the wrapper through
# the source-local renamed symbol.
set_source_files_properties(
    "seismic_recorder.c"
    PROPERTIES
        COMPILE_DEFINITIONS
            "seismic_recorder_start=seismic_recorder_start_unleased;seismic_recorder_json_write_metadata=seismic_recorder_lifetime_write_metadata"
)

idf_component_register(
    SRCS
        "seismic_recorder.c"
        "seismic_recorder_object_lifetime.c"
        "seismic_recorder_start_policy.c"
        "seismic_recorder_storage_preflight.c"
        "seismic_recorder_delete.c"
        "seismic_recorder_fs.c"
        "seismic_recorder_json.c"
        "seismic_recorder_reader.c"
        "seismic_recorder_waveform.c"
        "seismic_recorder_lod.c"
        "seismic_recorder_lod_reader.c"
        "seismic_recorder_lod_store.c"
        "seismic_recorder_cache.c"
        "seismic_recorder_list_responsive.c"
        "seismic_recorder_recovery.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        object_store
        runtime_observability
        sd_card
        seismic_acquisition
        seismic_buffer
        storage_manager
        freertos
        json
        log
        esp_timer
)
