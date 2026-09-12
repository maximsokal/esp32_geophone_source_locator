set(INSIGHTS_BRIDGE_SRCS
    "insights_bridge.c"
)

if(CONFIG_APP_ESP_INSIGHTS_TEST_AUTH_PROVIDER)
    list(APPEND INSIGHTS_BRIDGE_SRCS "insights_test_auth_provider.c")
endif()

idf_component_register(
    SRCS ${INSIGHTS_BRIDGE_SRCS}

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        app_events
        heap
        log
        runtime_observability
        freertos

    PRIV_REQUIRES
        esp_diagnostics
        esp_insights
)
