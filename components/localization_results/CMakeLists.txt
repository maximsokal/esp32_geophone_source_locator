idf_component_register(
    SRCS
        "localization_run_record_json.c"
        "localization_event_run_record.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        localization
        json
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
