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

set(UI_RENDER_SOURCES
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
    "${REPO_ROOT}/nRF54L15/src/nrf_cs_scheduler.c"
    "${REPO_ROOT}/components/ui/ui_workflow_sensor_values.c"
    "${REPO_ROOT}/components/ui_i18n/ui_utf8_format.c"
    "${REPO_ROOT}/components/ui/fonts/ui_font_montserrat_14.c"
    "${REPO_ROOT}/components/ui_i18n/ui_i18n.c"
    "${REPO_ROOT}/components/ui_i18n/ui_i18n_catalog.gen.c"
    "${REPO_ROOT}/components/workflow_service/workflow_core.c"
    "${REPO_ROOT}/components/result_view/result_view.c"
    "${REPO_ROOT}/components/seismic_buffer/seismic_envelope.c"
    "${REPO_ROOT}/components/seismic_acquisition/seismic_rate_profile.c"
    "${REPO_ROOT}/components/localization_job/localization_material_presets.c")
add_executable(ui_render ${UI_RENDER_SOURCES})
add_executable(ui_navigation_render ${UI_RENDER_SOURCES} ui_navigation_harness.c
    "${REPO_ROOT}/components/ui/ui_screen_registry.c")
target_compile_definitions(ui_navigation_render PRIVATE UI_NATIVE_FULL_SCREENS=1)
add_executable(ui_lvgl_wait ui_lvgl_wait_test.c
    "${REPO_ROOT}/components/ui/ui_lvgl.c"
    "${REPO_ROOT}/components/ui/ui_perf_policy.c")
target_include_directories(ui_lvgl_wait BEFORE PRIVATE lvgl_driver_stubs)
enable_testing()
add_test(NAME ui_lvgl_wait COMMAND ui_lvgl_wait)
file(GLOB COMPONENT_INCLUDES "${REPO_ROOT}/components/*/include")
if(MSVC)
    target_compile_options(lvgl PRIVATE /W0)
endif()
foreach(UI_TARGET IN ITEMS ui_render ui_navigation_render ui_lvgl_wait)
    target_include_directories(${UI_TARGET} PRIVATE stubs ${COMPONENT_INCLUDES} "${REPO_ROOT}/components/ui"
        "${REPO_ROOT}/nRF54L15/include" "${REPO_ROOT}/tests/host/stubs/object_store_profile")
    target_link_libraries(${UI_TARGET} PRIVATE lvgl::lvgl)
    target_compile_definitions(${UI_TARGET} PRIVATE CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING=1)
    if(MSVC)
        target_compile_options(${UI_TARGET} PRIVATE /W4 /WX /utf-8 /UNDEBUG /Gy /experimental:c11atomics)
        target_compile_definitions(${UI_TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_options(${UI_TARGET} PRIVATE /OPT:REF)
    else()
        target_compile_options(${UI_TARGET} PRIVATE -Wall -Wextra -Werror -UNDEBUG -ffunction-sections -fdata-sections)
        target_link_options(${UI_TARGET} PRIVATE -Wl,--gc-sections)
        target_link_libraries(${UI_TARGET} PRIVATE m)
    endif()
endforeach()
