idf_component_register(
    SRCS
        "web_server.c"
        "web_server_routes.c"
        "web_static_assets.c"
        "web_static_asset_data.c"
        "web_page.c"
        "web_api_status.c"
        "web_status_json.c"
        "web_status_json_pods.c"
        "web_json_writer.c"
        "web_seismic.c"
        "web_seismic_api.c"
        "web_seismic_page.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        device_status
        seismic_buffer
        esp_http_server
        esp_timer
        json
        log

    EMBED_TXTFILES
        "assets/index.html"
        "assets/dashboard.css"
        "assets/dashboard.js"
        "assets/seismic.html"
        "assets/seismic.css"
        "assets/seismic.js"
)
