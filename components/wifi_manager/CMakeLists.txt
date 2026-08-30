idf_component_register(
    SRCS
        "wifi_manager.c"
        "wifi_manager_mode.c"
        "wifi_manager_config.c"
        "wifi_manager_status.c"
        "wifi_manager_platform.c"
        "wifi_manager_credentials.c"
        "wifi_manager_profile.c"
        "wifi_manager_runtime.c"
        "wifi_manager_runtime_format.c"
        "wifi_manager_sta.c"
        "wifi_manager_sta_events.c"
        "wifi_manager_setup_ap.c"
        "wifi_manager_setup_events.c"
        "wifi_manager_mdns.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        app_event_bus
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        freertos
        heap
        log
        mdns
)
