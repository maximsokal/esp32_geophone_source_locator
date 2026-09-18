# Recorder lifetime ownership is interposed only at the public START boundary
# and terminal metadata call inside seismic_recorder.c. The original recorder
# state machine stays unchanged and remains available to the wrapper through
# the source-local renamed symbol.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    set_source_files_properties(
        "seismic_recorder.c"
        PROPERTIES
            COMPILE_DEFINITIONS
                "seismic_recorder_start=seismic_recorder_start_unleased;seismic_recorder_json_write_metadata=seismic_recorder_lifetime_write_metadata"
    )

    # MAX-70 keeps the mature recorder I/O translation unit untouched while the
    # deterministic archive-list implementation lives in its own focused source.
    # Rename only the legacy list symbol in seismic_recorder_fs.c; all other FS
    # create/write/flush/delete behavior remains byte-for-byte on the old path.
    set_source_files_properties(
        "seismic_recorder_fs.c"
        PROPERTIES
            COMPILE_DEFINITIONS
                "seismic_recorder_fs_list_sessions=seismic_recorder_fs_list_sessions_legacy"
    )
endif()

idf_component_register(
    SRCS
        "seismic_recorder.c"
        "seismic_recorder_object_lifetime.c"
        "seismic_recorder_start_policy.c"
        "seismic_recorder_storage_preflight.c"
        "seismic_recorder_delete.c"
        "seismic_recorder_fs.c"
        "seismic_recorder_fs_list.c"
        "seismic_recorder_geometry_policy.c"
        "seismic_recorder_session_selection.c"
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
        nrf_geometry_solver
        nrf_topology_service
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
