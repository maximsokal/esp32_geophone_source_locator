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
        localization_extended
        localization_results
        localization_run_store
        localization_semblance
        localization_traveltime_inversion
        localization_velocity_free
        object_store
        scan_occupancy
        storage_manager
        seismic_dsp
        seismic_recorder
)
