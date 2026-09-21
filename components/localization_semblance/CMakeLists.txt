idf_component_register(
    SRCS
        "localization_semblance.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
        localization
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
