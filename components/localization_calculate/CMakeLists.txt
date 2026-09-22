idf_component_register(
    SRCS
        "localization_calculate_policy.c"
        "localization_calculate.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        geophone_events
        localization
        localization_comparison
        localization_energy_baseline
        localization_results
        localization_run_store
        localization_semblance
        localization_traveltime_inversion
        object_store
        seismic_dsp
        seismic_recorder
)
