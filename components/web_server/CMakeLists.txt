set(WEB_SERVER_TLS_CERT_FILE
    "${CMAKE_CURRENT_LIST_DIR}/certs/servercert.pem"
)

set(WEB_SERVER_TLS_KEY_FILE
    "${CMAKE_CURRENT_LIST_DIR}/certs/prvtkey.pem"
)

set(WEB_SERVER_ROOT_CA_FILE
    "${CMAKE_CURRENT_LIST_DIR}/certs/rootCA.crt"
)

set(WEB_SERVER_WEBUI_PROJECT_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../webui"
)

set(WEB_SERVER_WEBUI_SOURCE_DIR
    "${WEB_SERVER_WEBUI_PROJECT_DIR}/esp-dist"
)

set(WEB_SERVER_WEBUI_GENERATED_INDEX
    "${CMAKE_CURRENT_LIST_DIR}/generated_webui_index.html"
)

set(WEB_SERVER_WEBUI_GENERATED_JS
    "${CMAKE_CURRENT_LIST_DIR}/generated_webui_assets_app.js.gz"
)

set(WEB_SERVER_WEBUI_GENERATED_CSS
    "${CMAKE_CURRENT_LIST_DIR}/generated_webui_assets_app.css.gz"
)

set(WEB_SERVER_WEBUI_GENERATED_CHUNK_DIR
    "${CMAKE_CURRENT_LIST_DIR}/generated_webui_chunks"
)

set(WEB_SERVER_WEBUI_GENERATED_CHUNK_TABLE
    "${CMAKE_CURRENT_LIST_DIR}/generated_webui_chunk_table.inc"
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

# ESP-IDF evaluates component CMakeLists.txt once in CMake script mode only to
# discover dependencies. Do not run filesystem dependency tracking, npm, or
# generated-asset work in that discovery pass: CONFIGURE_DEPENDS is illegal in
# script mode and side effects there make requirement discovery brittle.
# The real configure pass below still builds the current React bundle and fails
# closed if it cannot produce a complete esp-dist.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    find_program(WEB_SERVER_NPM_EXECUTABLE NAMES npm.cmd npm)
    find_program(WEB_SERVER_NODE_EXECUTABLE NAMES node.exe node)
    if(NOT WEB_SERVER_NPM_EXECUTABLE OR NOT WEB_SERVER_NODE_EXECUTABLE)
        message(FATAL_ERROR
            "GeoLocator firmware requires Node.js/npm so the current WebUI can be built before embedding. Install Node.js 22+ and rerun idf.py reconfigure/build."
        )
    endif()

    file(GLOB_RECURSE WEB_SERVER_WEBUI_SOURCE_INPUTS CONFIGURE_DEPENDS
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/src/*.ts"
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/src/*.tsx"
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/src/*.css"
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/scripts/*.mjs"
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/stress/*.js"
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/stress/*.html"
    )

    set(WEB_SERVER_WEBUI_CONFIG_INPUTS
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/package.json"
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/package-lock.json"
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/index.html"
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/tsconfig.json"
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/vite.config.ts"
    )
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${WEB_SERVER_WEBUI_SOURCE_INPUTS}
        ${WEB_SERVER_WEBUI_CONFIG_INPUTS}
    )

    set(WEB_SERVER_WEBUI_NODE_LOCK
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/node_modules/.package-lock.json"
    )
    set(WEB_SERVER_WEBUI_PACKAGE_LOCK
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/package-lock.json"
    )
    set(WEB_SERVER_WEBUI_TSC_ENTRY
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/node_modules/typescript/bin/tsc"
    )
    set(WEB_SERVER_WEBUI_VITE_ENTRY
        "${WEB_SERVER_WEBUI_PROJECT_DIR}/node_modules/vite/bin/vite.js"
    )

    set(WEB_SERVER_WEBUI_NEEDS_NPM_CI 0)
    if(NOT EXISTS "${WEB_SERVER_WEBUI_NODE_LOCK}")
        set(WEB_SERVER_WEBUI_NEEDS_NPM_CI 1)
    elseif("${WEB_SERVER_WEBUI_PACKAGE_LOCK}" IS_NEWER_THAN "${WEB_SERVER_WEBUI_NODE_LOCK}")
        set(WEB_SERVER_WEBUI_NEEDS_NPM_CI 1)
    endif()

    if(WEB_SERVER_WEBUI_NEEDS_NPM_CI)
        message(STATUS "GeoLocator WebUI dependencies are missing/stale; running npm ci")
        execute_process(
            COMMAND "${WEB_SERVER_NPM_EXECUTABLE}" ci --no-audit --no-fund
            WORKING_DIRECTORY "${WEB_SERVER_WEBUI_PROJECT_DIR}"
            RESULT_VARIABLE WEB_SERVER_NPM_CI_RESULT
            OUTPUT_VARIABLE WEB_SERVER_NPM_CI_STDOUT
            ERROR_VARIABLE WEB_SERVER_NPM_CI_STDERR
        )
        if(NOT WEB_SERVER_NPM_CI_RESULT EQUAL 0)
            message(FATAL_ERROR
                "GeoLocator npm ci failed (${WEB_SERVER_NPM_CI_RESULT})\n${WEB_SERVER_NPM_CI_STDOUT}\n${WEB_SERVER_NPM_CI_STDERR}"
            )
        endif()
    endif()

    if(NOT EXISTS "${WEB_SERVER_WEBUI_TSC_ENTRY}")
        message(FATAL_ERROR
            "GeoLocator local TypeScript compiler is missing after npm ci: ${WEB_SERVER_WEBUI_TSC_ENTRY}"
        )
    endif()
    if(NOT EXISTS "${WEB_SERVER_WEBUI_VITE_ENTRY}")
        message(FATAL_ERROR
            "GeoLocator local Vite entry is missing after npm ci: ${WEB_SERVER_WEBUI_VITE_ENTRY}"
        )
    endif()

    # Run the package entry files with node directly. This intentionally avoids
    # node_modules/.bin shims because some Windows/npm configurations disable
    # bin-link creation even though the packages themselves are installed.
    message(STATUS "Type-checking current GeoLocator React WebUI for firmware embed")
    execute_process(
        COMMAND "${WEB_SERVER_NODE_EXECUTABLE}" "${WEB_SERVER_WEBUI_TSC_ENTRY}" --noEmit
        WORKING_DIRECTORY "${WEB_SERVER_WEBUI_PROJECT_DIR}"
        RESULT_VARIABLE WEB_SERVER_WEBUI_TSC_RESULT
        OUTPUT_VARIABLE WEB_SERVER_WEBUI_TSC_STDOUT
        ERROR_VARIABLE WEB_SERVER_WEBUI_TSC_STDERR
    )
    if(NOT WEB_SERVER_WEBUI_TSC_RESULT EQUAL 0)
        message(FATAL_ERROR
            "GeoLocator WebUI typecheck failed (${WEB_SERVER_WEBUI_TSC_RESULT})\n${WEB_SERVER_WEBUI_TSC_STDOUT}\n${WEB_SERVER_WEBUI_TSC_STDERR}"
        )
    endif()

    message(STATUS "Bundling current GeoLocator React WebUI for firmware embed")
    execute_process(
        COMMAND "${WEB_SERVER_NODE_EXECUTABLE}" "${WEB_SERVER_WEBUI_VITE_ENTRY}" build
        WORKING_DIRECTORY "${WEB_SERVER_WEBUI_PROJECT_DIR}"
        RESULT_VARIABLE WEB_SERVER_WEBUI_VITE_RESULT
        OUTPUT_VARIABLE WEB_SERVER_WEBUI_VITE_STDOUT
        ERROR_VARIABLE WEB_SERVER_WEBUI_VITE_STDERR
    )
    if(NOT WEB_SERVER_WEBUI_VITE_RESULT EQUAL 0)
        message(FATAL_ERROR
            "GeoLocator WebUI Vite build failed (${WEB_SERVER_WEBUI_VITE_RESULT})\n${WEB_SERVER_WEBUI_VITE_STDOUT}\n${WEB_SERVER_WEBUI_VITE_STDERR}"
        )
    endif()

    message(STATUS "Preparing GeoLocator ESP WebUI assets")
    execute_process(
        COMMAND "${WEB_SERVER_NODE_EXECUTABLE}" scripts/prepare-esp-assets.mjs
        WORKING_DIRECTORY "${WEB_SERVER_WEBUI_PROJECT_DIR}"
        RESULT_VARIABLE WEB_SERVER_WEBUI_PREPARE_RESULT
        OUTPUT_VARIABLE WEB_SERVER_WEBUI_PREPARE_STDOUT
        ERROR_VARIABLE WEB_SERVER_WEBUI_PREPARE_STDERR
    )
    if(NOT WEB_SERVER_WEBUI_PREPARE_RESULT EQUAL 0)
        message(FATAL_ERROR
            "GeoLocator WebUI ESP asset preparation failed (${WEB_SERVER_WEBUI_PREPARE_RESULT})\n${WEB_SERVER_WEBUI_PREPARE_STDOUT}\n${WEB_SERVER_WEBUI_PREPARE_STDERR}"
        )
    endif()

    file(REMOVE
        "${WEB_SERVER_WEBUI_GENERATED_INDEX}"
        "${WEB_SERVER_WEBUI_GENERATED_JS}"
        "${WEB_SERVER_WEBUI_GENERATED_CSS}"
        "${CMAKE_CURRENT_LIST_DIR}/generated_webui_app.js.gz"
        "${CMAKE_CURRENT_LIST_DIR}/generated_webui_app.css.gz"
    )

    file(REMOVE_RECURSE
        "${CMAKE_CURRENT_LIST_DIR}/generated_webui"
        "${WEB_SERVER_WEBUI_GENERATED_CHUNK_DIR}"
    )
    file(REMOVE
        "${WEB_SERVER_WEBUI_GENERATED_CHUNK_TABLE}"
    )

    if(
        EXISTS "${WEB_SERVER_WEBUI_SOURCE_DIR}/index.html"
        AND EXISTS "${WEB_SERVER_WEBUI_SOURCE_DIR}/build-id.txt"
        AND EXISTS "${WEB_SERVER_WEBUI_SOURCE_DIR}/generated_webui_chunk_table.inc"
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

        configure_file(
            "${WEB_SERVER_WEBUI_SOURCE_DIR}/generated_webui_chunk_table.inc"
            "${WEB_SERVER_WEBUI_GENERATED_CHUNK_TABLE}"
            COPYONLY
        )

        file(MAKE_DIRECTORY
            "${WEB_SERVER_WEBUI_GENERATED_CHUNK_DIR}"
        )
        file(GLOB WEB_SERVER_WEBUI_CHUNK_SOURCES
            "${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/*.js.gz"
            "${WEB_SERVER_WEBUI_SOURCE_DIR}/assets/*.css.gz"
        )
        list(SORT WEB_SERVER_WEBUI_CHUNK_SOURCES)
        set(WEB_SERVER_WEBUI_CHUNK_COUNT 0)
        foreach(WEB_SERVER_WEBUI_CHUNK_SOURCE IN LISTS WEB_SERVER_WEBUI_CHUNK_SOURCES)
            get_filename_component(
                WEB_SERVER_WEBUI_CHUNK_NAME
                "${WEB_SERVER_WEBUI_CHUNK_SOURCE}"
                NAME
            )
            if(
                WEB_SERVER_WEBUI_CHUNK_NAME STREQUAL "app.js.gz"
                OR WEB_SERVER_WEBUI_CHUNK_NAME STREQUAL "app.css.gz"
            )
                continue()
            endif()
            math(EXPR WEB_SERVER_WEBUI_CHUNK_COUNT "${WEB_SERVER_WEBUI_CHUNK_COUNT} + 1")
            configure_file(
                "${WEB_SERVER_WEBUI_CHUNK_SOURCE}"
                "${WEB_SERVER_WEBUI_GENERATED_CHUNK_DIR}/${WEB_SERVER_WEBUI_CHUNK_NAME}"
                COPYONLY
            )
            list(APPEND WEB_SERVER_WEBUI_EMBED_FILES
                "generated_webui_chunks/${WEB_SERVER_WEBUI_CHUNK_NAME}"
            )
        endforeach()
        if(WEB_SERVER_WEBUI_CHUNK_COUNT EQUAL 0)
            message(FATAL_ERROR
                "GeoLocator WebUI split build produced no secondary JS/CSS assets"
            )
        endif()

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
            "generated_webui_assets_app.js.gz"
            "generated_webui_assets_app.css.gz"
        )

        set(WEB_SERVER_WEBUI_SEED_EMBEDDED 1)
    else()
        message(FATAL_ERROR
            "GeoLocator WebUI build completed but esp-dist is incomplete; refusing to embed stale/missing browser assets"
        )
    endif()
endif()

idf_component_register(
    SRCS
        "web_server.c"
        "web_server_lifecycle_policy.c"
        "web_route_capacity.c"
        "web_server_prepare.c"
        "web_server_routes.c"
        "web_static_asset_data.c"
        "web_webui.c"
        "web_webui_flash.c"
        "web_page.c"
        "web_api_status.c"
        "web_api_control.c"
        "web_scan_store.c"
        "web_client_log.c"
        "web_status_json.c"
        "web_status_policy.c"
        "web_status_cache_arbitration.c"
        "web_status_json_objects.c"
        "web_status_json_recording.c"
        "web_status_json_pods.c"
        "web_json_writer.c"
        "web_seismic.c"
        "web_seismic_api.c"
        "web_seismic_page.c"
        "web_localization_api.c"
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
        app_events
        device_status
        localization_calculate
        localization_job
        localization_results
        localization_run_store
        measurement_service
        nrf_topology_service
        object_store
        power_control
        recording_gate
        runtime_observability
        sd_card
        seismic_acquisition
        seismic_buffer
        seismic_recorder
        storage_manager
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

if(NOT CMAKE_SCRIPT_MODE_FILE)
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

    message(STATUS
        "GeoLocator React Web UI seed will be embedded: ${WEB_SERVER_WEBUI_BUILD_ID}"
    )
endif()
