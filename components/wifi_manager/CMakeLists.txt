idf_component_register(
    SRCS
        "wifi_manager.c"
        "wifi_manager_dhcp.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        log
)
