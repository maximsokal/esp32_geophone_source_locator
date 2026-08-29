idf_component_register(
    SRCS
        "localization.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
