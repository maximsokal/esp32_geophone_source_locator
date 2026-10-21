cmake_minimum_required(VERSION 3.20)
project(geophone_ui_render C CXX)
set(CMAKE_C_STANDARD 11)
get_filename_component(REPO_ROOT "../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(LV_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO_ROOT}/managed_components/lvgl__lvgl" lvgl)

add_executable(ui_render
    ui_render_harness.c
    ui_render_fakes.c
    ui_render_menus.c
    "${REPO_ROOT}/components/ui/ui_shell.c"
    "${REPO_ROOT}/components/ui/ui_home.c"
    "${REPO_ROOT}/components/ui/ui_objects.c"
    "${REPO_ROOT}/components/ui/ui_objects_input.c"
    "${REPO_ROOT}/components/ui/ui_objects_delete_model.c"
    "${REPO_ROOT}/components/ui/ui_storage.c"
    "${REPO_ROOT}/components/ui/ui_storage_input.c"
    "${REPO_ROOT}/components/ui/ui_wifi.c"
    "${REPO_ROOT}/components/ui/ui_wifi_input.c"
    "${REPO_ROOT}/components/ui/ui_system.c"
    "${REPO_ROOT}/components/ui/ui_system_model.c"
    "${REPO_ROOT}/components/ui/ui_hardware.c"
    "${REPO_ROOT}/components/ui/ui_hardware_model.c"
    "${REPO_ROOT}/components/ui/ui_boot_error.c"
    "${REPO_ROOT}/components/ui/ui_navigation.c"
    "${REPO_ROOT}/components/ui/ui_preview3d.c"
    "${REPO_ROOT}/components/ui/ui_scan_preview_policy.c"
    "${REPO_ROOT}/components/scan_preview/scan_preview.c"
    "${REPO_ROOT}/components/scan_preview/scan_preview_state.c"
    "${REPO_ROOT}/components/scan_occupancy/scan_occupancy.c"
    "${REPO_ROOT}/components/localization_calculate/localization_calculate_policy.c"
    "${REPO_ROOT}/components/geophone_events/geophone_event_cross_correlation.c"
    "${REPO_ROOT}/tests/host/stubs/object_store_profile/cJSON.c"
    "${REPO_ROOT}/components/ui/ui_measurements.c"
    "${REPO_ROOT}/components/ui/ui_measurement_trace.c"
    "${REPO_ROOT}/components/ui/ui_measurements_nodes.c"
    "${REPO_ROOT}/components/ui/ui_node_link_status.c"
    "${REPO_ROOT}/components/nrf_topology_service/nrf_node_link_status_policy.c"
    "${REPO_ROOT}/components/ui/ui_perf_policy.c"
    "${REPO_ROOT}/components/ui/ui_power_badge.c"
    "${REPO_ROOT}/components/ui/ui_workflow_model.c"
    "${REPO_ROOT}/components/ui_i18n/ui_utf8_format.c"
    "${REPO_ROOT}/components/ui/fonts/ui_font_montserrat_14.c"
    "${REPO_ROOT}/components/ui_i18n/ui_i18n.c"
    "${REPO_ROOT}/components/ui_i18n/ui_i18n_catalog.gen.c"
    "${REPO_ROOT}/components/workflow_service/workflow_core.c"
    "${REPO_ROOT}/components/result_view/result_view.c"
    "${REPO_ROOT}/components/seismic_buffer/seismic_envelope.c"
    "${REPO_ROOT}/components/localization_job/localization_material_presets.c")
file(GLOB COMPONENT_INCLUDES "${REPO_ROOT}/components/*/include")
target_include_directories(ui_render PRIVATE stubs ${COMPONENT_INCLUDES} "${REPO_ROOT}/components/ui"
    "${REPO_ROOT}/nRF54L15/include" "${REPO_ROOT}/tests/host/stubs/object_store_profile")
target_link_libraries(ui_render PRIVATE lvgl::lvgl)
target_compile_definitions(ui_render PRIVATE CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING=1)
if(MSVC)
    target_compile_options(lvgl PRIVATE /W0)
    target_compile_options(ui_render PRIVATE /W4 /WX /utf-8 /UNDEBUG /Gy /experimental:c11atomics)
    target_compile_definitions(ui_render PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_options(ui_render PRIVATE /OPT:REF)
else()
    target_compile_options(ui_render PRIVATE -Wall -Wextra -Werror -UNDEBUG -ffunction-sections -fdata-sections)
    target_link_options(ui_render PRIVATE -Wl,--gc-sections)
    target_link_libraries(ui_render PRIVATE m)
endif()
