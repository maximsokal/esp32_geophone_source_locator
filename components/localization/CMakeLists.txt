idf_component_register(
    SRCS
        "localization.c"
        "localization_event_bridge.c"
        "localization_dimensionality_policy.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
        geophone_events
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
