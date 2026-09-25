idf_component_register(
    SRCS
        "localization_vf_math.c"
        "localization_tdcas.c"
        "localization_psafuvs.c"
        "localization_suea.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        esp_common
        localization
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
