idf_component_register(
    SRCS
        "web_server.c"
        "web_page.c"
        "web_api_status.c"
        "web_status_json.c"
        "web_status_json_pods.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        device_status
        esp_http_server
        esp_timer
        json
        log
)