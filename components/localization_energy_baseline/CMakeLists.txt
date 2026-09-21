idf_component_register(
    SRCS
        "localization_energy_baseline.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
        localization
)
