idf_component_register(
    SRCS
        "localization_run_record_json.c"
        "localization_event_run_record.c"
        "localization_pick_record.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        geophone_events
        localization
        json
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
