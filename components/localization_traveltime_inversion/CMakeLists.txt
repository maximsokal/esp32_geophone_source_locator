idf_component_register(
    SRCS
        "localization_traveltime_inversion.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
        localization
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
