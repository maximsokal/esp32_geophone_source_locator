idf_component_register(
    SRCS
        "localization_ext_fit.c"
        "localization_robust.c"
        "localization_psp.c"
        "localization_xcorr_tdoa.c"
        "localization_eikonal.c"
        "localization_bayes.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        esp_common
        localization
        localization_velocity_free
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
