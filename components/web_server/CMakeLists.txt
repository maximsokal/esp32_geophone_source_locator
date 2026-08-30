set(WEB_SERVER_TLS_CERT_FILE
    "${CMAKE_CURRENT_LIST_DIR}/certs/servercert.pem"
)

set(WEB_SERVER_TLS_KEY_FILE
    "${CMAKE_CURRENT_LIST_DIR}/certs/prvtkey.pem"
)

set(WEB_SERVER_TLS_EMBED_FILES)
set(WEB_SERVER_TLS_CREDENTIALS_EMBEDDED 0)

if(
    EXISTS "${WEB_SERVER_TLS_CERT_FILE}"
    AND EXISTS "${WEB_SERVER_TLS_KEY_FILE}"
)
    list(APPEND WEB_SERVER_TLS_EMBED_FILES
        "certs/servercert.pem"
        "certs/prvtkey.pem"
    )

    set(WEB_SERVER_TLS_CREDENTIALS_EMBEDDED 1)
endif()

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
        "web_wifi_setup.c"
        "web_wifi_setup_page.c"
        "web_wifi_setup_request.c"
        "web_wifi_setup_form.c"
        "web_wifi_setup_decode.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        app_event_bus
        device_status
        seismic_buffer
        wifi_manager
        esp_http_server
        esp_https_server
        esp_event
        esp_netif
        esp_wifi
        esp_timer
        freertos
        heap
        json
        log

    EMBED_TXTFILES
        "assets/index.html"
        "assets/dashboard.css"
        "assets/dashboard.js"
        "assets/seismic.html"
        "assets/seismic.css"
        "assets/seismic.js"
        ${WEB_SERVER_TLS_EMBED_FILES}
)

target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
        WEB_SERVER_TLS_CREDENTIALS_EMBEDDED=${WEB_SERVER_TLS_CREDENTIALS_EMBEDDED}
)

if(WEB_SERVER_TLS_CREDENTIALS_EMBEDDED)
    message(STATUS "GeoLocator HTTPS credentials will be embedded")
else()
    message(WARNING
        "GeoLocator HTTPS credentials are missing; HTTP setup will build, but HTTPS will stay disabled until certs/servercert.pem and certs/prvtkey.pem are generated"
    )
endif()
