idf_component_register(
    SRCS
        "insights_bridge.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        heap
        log
        runtime_observability

    PRIV_REQUIRES
        esp_diagnostics
        esp_insights
)
