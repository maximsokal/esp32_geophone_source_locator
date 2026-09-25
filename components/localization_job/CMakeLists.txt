idf_component_register(
    SRCS
        "localization_job_state.c"
        "localization_job.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        esp_timer
        freertos
        heap
        json
        localization_calculate
        log
)
