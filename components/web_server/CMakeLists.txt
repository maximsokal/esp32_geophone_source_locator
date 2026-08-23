idf_component_register(
    SRCS
        "web_server.c"
        "web_page.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        esp_http_server
        log
)