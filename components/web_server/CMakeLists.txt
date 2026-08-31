set(WEB_SERVER_TLS_CERT_FILE
    "${CMAKE_CURRENT_LIST_DIR}/certs/servercert.pem"
)

set(WEB_SERVER_TLS_KEY_FILE
    "${CMAKE_CURRENT_LIST_DIR}/certs/prvtkey.pem"
)

set(WEB_SERVER_ROOT_CA_FILE
    "${CMAKE_CURRENT_LIST_DIR}/certs/rootCA.crt"
)

set(WEB_SERVER_WEBUI_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../webui/esp-dist"
)

set(WEB_SERVER_WEBUI_GENERATED_INDEX
    "${CMAKE_CURRENT_LIST_DIR}/generated_webui_index.html"
)

set(WEB_SERVER_WEBUI_GENERATED_JS
    "${CMAKE_CURRENT_LIST_DIR}/generated_webui_app.js.gz"
)

set(WEB_SERVER_WEBUI_GENERATED_CSS
    "${CMAKE_CURRENT_LIST_DIR}/generated_webui_app.css.gz"
)

set(WEB_SERVER_TLS_EMBED_FILES)
set(WEB_SERVER_WEBUI_EMBED_FILES)
set(WEB_SERVER_WEBUI_EMBED_TXTFILES)
set(WEB_SERVER_TLS_CREDENTIALS_EMBEDDED 0)
set(WEB_SERVER_ROOT_CA_EMBEDDED 0)
set(WEB_SERVER_WEBUI_SEED_EMBEDDED 0)
set(WEB_SERVER_WEBUI_BUILD_ID "none")

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

if(EXISTS "${WEB_SERVER_ROOT_CA_FILE}")
    list(APPEND WEB_SERVER_TLS_EMBED_FILES
        "certs/rootCA.crt"
    )

    set(WEB_SERVER_ROOT_CA_EMBEDDED 1)
endif()

# Keep generated seed filenames flat and stable. ESP-IDF derives the linker
# symbols from the exact EMBED_FILES/EMBED_TXTFILES strings, so directory
# components would become part of the symbol name as underscores.
file(REMOVE
    "${WEB_SERVER_WEBUI_GENERATED_INDEX}"
    "${WEB_SERVER_WEBUI_GENERATED_JS}"
    "${WEB_SERVER_WEBUI_GENERATED_CSS}"
)

# Clean up the first WEB-HTTPS-B implementation's generated directory if it
# exists in a local checkout.
file(REMOVE_RECURSE
    "${CMAKE_CURRENT_LIST_DIR}/generated_webui"
)

if(
    EXISTS "${WEB_SERVER_WEBUI_SOURCE_DIR}/index.html"
    AND EXISTS "${WEB_SERVER_WEBUI_SOURCE_DIR}/build-id.txt"
    AND EXISTS "${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/app.js.gz"
    AND EXISTS "${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/app.css.gz"
)
    configure_file(
        "${WEB_SERVER_WEBUI_SOURCE_DIR}/index.html"
        "${WEB_SERVER_WEBUI_GENERATED_INDEX}"
        COPYONLY
    )

    configure_file(
        "${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/app.js.gz"
        "${WEB_SERVER_WEBUI_GENERATED_JS}"
        COPYONLY
    )

    configure_file(
        "${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/app.css.gz"
        "${WEB_SERVER_WEBUI_GENERATED_CSS}"
        COPYONLY
    )

    file(READ
        "${WEB_SERVER_WEBUI_SOURCE_DIR}/build-id.txt"
        WEB_SERVER_WEBUI_BUILD_ID
    )

    string(STRIP
        "${WEB_SERVER_WEBUI_BUILD_ID}"
        WEB_SERVER_WEBUI_BUILD_ID
    )

    list(APPEND WEB_SERVER_WEBUI_EMBED_TXTFILES
        "generated_webui_index.html"
    )

    list(APPEND WEB_SERVER_WEBUI_EMBED_FILES
        "generated_webui_app.js.gz"
        "generated_webui_app.css.gz"
    )

    set(WEB_SERVER_WEBUI_SEED_EMBEDDED 1)
endif()

idf_component_register(
    SRCS
        "web_server.c"
        "web_server_routes.c"
        "web_static_assets.c"
        "web_static_asset_data.c"
        "web_webui.c"
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
        "web_https_onboarding.c"
        "web_https_onboarding_ui.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        app_event_bus
        device_status
        sd_card
        seismic_buffer
        wifi_manager
        esp_http_server
        esp_https_server
        esp_event
        esp_netif
        esp_wifi
        esp_timer
        esp_system
        freertos
        heap
        json
        log

    EMBED_FILES
        ${WEB_SERVER_WEBUI_EMBED_FILES}

    EMBED_TXTFILES
        "assets/index.html"
        "assets/dashboard.css"
        "assets/dashboard.js"
        "assets/seismic.html"
        "assets/seismic.css"
        "assets/seismic.js"
        ${WEB_SERVER_TLS_EMBED_FILES}
        ${WEB_SERVER_WEBUI_EMBED_TXTFILES}
)

target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
        WEB_SERVER_TLS_CREDENTIALS_EMBEDDED=${WEB_SERVER_TLS_CREDENTIALS_EMBEDDED}
        WEB_SERVER_ROOT_CA_EMBEDDED=${WEB_SERVER_ROOT_CA_EMBEDDED}
        WEB_SERVER_WEBUI_SEED_EMBEDDED=${WEB_SERVER_WEBUI_SEED_EMBEDDED}
        WEB_SERVER_WEBUI_BUILD_ID=\"${WEB_SERVER_WEBUI_BUILD_ID}\"
)

if(WEB_SERVER_TLS_CREDENTIALS_EMBEDDED)
    message(STATUS "GeoLocator HTTPS credentials will be embedded")
else()
    message(WARNING
        "GeoLocator HTTPS credentials are missing; HTTP setup will build, but HTTPS will stay disabled until certs/servercert.pem and certs/prvtkey.pem are generated"
    )
endif()

if(WEB_SERVER_ROOT_CA_EMBEDDED)
    message(STATUS "GeoLocator public Root CA will be embedded for QR onboarding")
else()
    message(WARNING
        "GeoLocator Root CA is missing; CA onboarding download will be unavailable until certs/rootCA.crt is generated"
    )
endif()

if(WEB_SERVER_WEBUI_SEED_EMBEDDED)
    message(STATUS
        "GeoLocator React Web UI seed will be embedded: ${WEB_SERVER_WEBUI_BUILD_ID}"
    )
else()
    message(WARNING
        "GeoLocator React Web UI seed is missing; run npm run build in webui before idf.py reconfigure/build to enable WEB-HTTPS-B"
    )
endif()
