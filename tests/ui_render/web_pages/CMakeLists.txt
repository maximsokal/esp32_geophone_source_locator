cmake_minimum_required(VERSION 3.20)
project(geophone_web_pages C)
set(CMAKE_C_STANDARD 11)
get_filename_component(REPO_ROOT "../../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(WEB "${REPO_ROOT}/components/web_server")
add_executable(web_pages
    ../web_pages_harness.c
    "${WEB}/web_wifi_setup_page.c"
    "${WEB}/web_wifi_setup.c"
    "${WEB}/web_wifi_setup_request.c"
    "${WEB}/web_wifi_setup_form.c"
    "${WEB}/web_wifi_setup_decode.c"
    "${WEB}/web_https_onboarding_ui.c"
    "${WEB}/web_https_onboarding.c"
    "${REPO_ROOT}/tests/host/stubs/object_store_profile/cJSON.c"
    "${WEB}/web_page.c"
    "${WEB}/web_seismic_page.c"
    "${WEB}/web_server_lifecycle_policy.c")
target_include_directories(web_pages PRIVATE stubs "${REPO_ROOT}/components/app_config/include"
    "${REPO_ROOT}/components/app_events/include" "${REPO_ROOT}/tests/host/stubs/object_store_profile"
    "${REPO_ROOT}/tests/host/stubs/seismic_pipeline" "${WEB}")
target_compile_definitions(web_pages PRIVATE WEB_SERVER_ROOT_CA_EMBEDDED=0)
if(MSVC)
    target_compile_definitions(web_pages PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(web_pages PRIVATE /W4 /WX /utf-8 /UNDEBUG /FIweb_pages_compat.h)
else()
    target_compile_options(web_pages PRIVATE -Wall -Wextra -Werror -UNDEBUG -include web_pages_compat.h)
endif()
