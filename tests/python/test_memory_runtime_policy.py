"""Static contracts for the ESP32 memory/runtime observability policy.

These checks protect configuration and integration intent only.  They do not
claim to emulate FreeRTOS scheduling, PSRAM allocation, SPI DMA, mbedTLS, or
the physical board.  Those properties require the target build and HIL logs.
"""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (REPO_ROOT / relative).read_text(encoding="utf-8")


def test_sdkconfig_defaults_keep_hybrid_tls_and_enable_runtime_stats():
    defaults = read("sdkconfig.defaults")
    tls_defaults = read("sdkconfig.tls.defaults")
    cmake = read("CMakeLists.txt")

    # sdkconfig.tls.defaults is intentionally the last defaults fragment, so it
    # overrides older generated values still present in sdkconfig.defaults.
    assert '"sdkconfig.defaults;sdkconfig.storage.defaults;sdkconfig.lvgl.defaults;sdkconfig.tls.defaults"' in cmake

    assert "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048" in tls_defaults
    assert "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536" in tls_defaults
    assert "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y" in tls_defaults
    assert "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y" in tls_defaults
    assert "# CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC is not set" in tls_defaults
    assert "# CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC is not set" in tls_defaults
    assert "# CONFIG_MBEDTLS_DYNAMIC_BUFFER is not set" in tls_defaults

    # Fragment sizes and runtime statistics still come from the generated base
    # config. The later TLS fragment changes allocator policy, not protocol size.
    assert "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384" in defaults
    assert "CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096" in defaults
    assert "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y" in defaults
    assert "CONFIG_FREERTOS_USE_TRACE_FACILITY=y" in defaults
    assert "CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=y" in defaults
    assert "CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y" in defaults
    assert "CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y" in defaults
    assert "CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y" in defaults


def test_build_guard_covers_tls_and_runtime_stats_contract():
    guard = read("main/app_build_config_guard.c")

    for option in (
        "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC",
        "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC",
        "CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC",
        "CONFIG_MBEDTLS_DYNAMIC_BUFFER",
        "CONFIG_FREERTOS_USE_TRACE_FACILITY",
        "CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS",
        "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS",
        "CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID",
        "CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64",
        "CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER",
    ):
        assert option in guard

    assert "runtime_observability" in read("main/CMakeLists.txt")


def test_runtime_observability_uses_raw_task_status_without_formatted_stats():
    source = read("components/runtime_observability/runtime_observability.c")

    assert "uxTaskGetSystemState(" in source
    assert "ulTaskGetIdleRunTimeCounterForCore(" in source
    assert "RUNTIME_CPU stage=" in source
    assert "RUNTIME_TASK rank=" in source
    assert "MEM_RUNTIME stage=" in source
    assert "RUNTIME_OBSERVABILITY_PERIOD_MS 10000U" in source
    assert "RUNTIME_OBSERVABILITY_TOP_TASKS 10U" in source
    assert "runtime_task_usage_insert" in source
    assert "runtime_task_is_interesting" not in source
    assert "needles[]" not in source
    assert "vTaskGetRunTimeStats(" not in source
    assert "TaskStatus_t *s_previous_status" in source
    assert "runtime_observability_request_snapshot" in source


def test_service_stacks_use_psram_caps_without_changing_affinity():
    expected = {
        "components/storage_service/storage_service.c": "APP_TASK_STACK_STORAGE_SERVICE",
        "components/object_service/object_service.c": "APP_TASK_STACK_OBJECT_SERVICE",
        "components/measurement_service/measurement_service.c": "APP_TASK_STACK_MEASUREMENT_SERVICE",
    }
    for relative, stack_macro in expected.items():
        source = read(relative)
        assert "xTaskCreatePinnedToCoreWithCaps(" in source
        assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in source
        assert "xTaskCreatePinnedToCore(" not in source
        assert "APP_TASK_CORE_SERVICES" in source
        assert stack_macro in source

    assert "vTaskDeleteWithCaps(" in read("components/storage_service/storage_service.c")
    assert "vTaskDeleteWithCaps(" in read("components/object_service/object_service_worker.c")
    assert "vTaskDeleteWithCaps(" in read("components/measurement_service/measurement_service.c")

    config = read("components/app_config/include/app_task_config.h")
    assert "APP_TASK_STACK_STORAGE_SERVICE 8192" in config
    assert "APP_TASK_STACK_OBJECT_SERVICE  8192" in config
    assert "APP_TASK_STACK_MEASUREMENT_SERVICE  8192" in config


def test_realtime_and_recorder_stacks_remain_on_original_path():
    config = read("components/app_config/include/app_task_config.h")
    assert "APP_TASK_STACK_AD7606          4096" in config
    assert "APP_TASK_STACK_SEISMIC_RECORDER  4096" in config
    assert "APP_TASK_CORE_REALTIME      1" in config

    recorder = read("components/seismic_recorder/seismic_recorder.c")
    acquisition = read("components/ad7606_acquisition/ad7606_acquisition.c")
    assert "xTaskCreatePinnedToCoreWithCaps(" not in recorder
    assert "xTaskCreatePinnedToCoreWithCaps(" not in acquisition


def test_https_and_tft_policies_preserve_payload_and_dma_safety():
    web_server = read("components/web_server/web_server.c")
    web_asset = read("components/web_server/web_webui_flash.c")
    tft = read("components/ui/ui_lvgl.c")

    assert "WEB_HTTPS_MAX_CLIENTS           2U" in web_server
    assert '"Connection",\n            "close"' not in web_asset
    assert '"Connection: close"' not in web_asset
    assert "#define WEB_FLASH_SEND_CHUNK_BYTES  2048U" in read("components/web_server/web_webui_flash.c")
    assert "DMA_RUNTIME owner=tft location=internal" in tft
    assert "#define UI_LVGL_BUFFER_ROWS  8" in tft
    assert "single and internal" in tft


def test_tft_flush_wait_blocks_on_dma_completion_instead_of_spinning():
    tft = read("components/ui/ui_lvgl.c")

    # LVGL 9 otherwise falls back to while (disp->flushing), which burns the
    # LVGL core while the SPI DMA transaction is still in flight. The tracked
    # app bridge must replace the port callback and wait on an ISR semaphore.
    assert "xSemaphoreCreateBinaryStatic(" in tft
    assert "xSemaphoreGiveFromISR(" in tft
    assert "xSemaphoreTake(" in tft
    assert "lv_display_set_flush_wait_cb(" in tft
    assert "esp_lcd_panel_io_register_event_callbacks(" in tft
    assert "UI_FLUSH_PERF:" in tft


def test_runtime_census_reports_unfiltered_top_tasks():
    runtime = read("components/runtime_observability/runtime_observability.c")

    # The census must not depend on guessed task-name needles. The top list is
    # built from every TaskStatus_t returned by uxTaskGetSystemState(), so the
    # next board log can identify taskLVGL/httpd/recorder or a new consumer.
    assert "uxTaskGetSystemState(" in runtime
    assert "runtime_task_usage_insert(" in runtime
    assert "const TaskStatus_t *status = &s_current_status[i];" in runtime
    assert "pcTaskName != NULL ? status->pcTaskName : \"?\"" in runtime


def test_status_objects_and_sessions_are_lifecycle_cache_only():
    objects = read("components/web_server/web_status_json_objects.c")
    recorder_fs = read("components/seismic_recorder/seismic_recorder_fs.c")
    recording = read("components/web_server/web_status_json_recording.c")
    status_json = read("components/web_server/web_status_json.c")
    status_api = read("components/web_server/web_api_status.c")
    internal = read("components/web_server/web_server_internal.h")

    assert "seismic_recorder_list_sessions(" in objects
    assert "web_status_json_objects_refresh(void)" in objects
    assert "xTaskCreatePinnedToCoreWithCaps(" in objects
    assert "xQueueOverwrite(" in objects
    assert "object_service_event_observer_t" in read("components/object_service/include/object_service.h")
    assert "web_status_json_objects_object_event" in objects
    assert "WEB_OBJECT_CACHE_TTL_US" not in objects
    assert "web_status_json_objects_cache_ready" in internal
    assert "opendir(sessions_path)" in recorder_fs
    assert "readdir(directory)" in recorder_fs
    assert "seismic_recorder_fs_session_metadata_present" in recorder_fs
    assert "storage_manager_path_exists" not in recorder_fs
    assert "web_status_json_objects_refresh" not in recording
    assert "seismic_recorder_list_sessions(" not in recording
    assert "object_store_list_profiles_with_defaults(" not in status_api
    assert "web_status_json_objects_cache_ready()" in status_json
    assert "storage_busy" in status_json
    assert "503 Service Unavailable" in status_api
    assert "from RAM/PSRAM cache; 503 while refreshing" in status_api


def test_measurement_service_gates_start_and_refreshes_after_storage_completion():
    header = read("components/measurement_service/include/measurement_service.h")
    source = read("components/measurement_service/measurement_service.c")
    web_server = read("components/web_server/web_server.c")
    web_control = read("components/web_server/web_api_control.c")

    assert "measurement_service_set_start_guard" in header
    assert "measurement_service_set_storage_refresh_callback" in header
    assert "s_start_guard" in source
    assert "s_storage_refresh_callback" in source
    assert "command_type == MEASUREMENT_COMMAND_START" in source
    assert "measurement_service_notify_storage_refresh();" in source
    status_cache_start = web_server[
        web_server.index("static void web_server_status_cache_start"):
        web_server.index("static void web_server_status_cache_stop")
    ]
    assert "web_status_json_objects_request_refresh();" in status_cache_start
    assert "web_status_json_objects_refresh();" not in status_cache_start
    assert "web_status_json_objects_start_guard" in web_server
    start_helper = web_control[
        web_control.index("static esp_err_t web_control_measurement_start"):
        web_control.index("static esp_err_t web_control_acquisition_stop")
    ]
    assert "web_status_json_objects_cache_ready()" not in start_helper
    assert "return measurement_service_start_object_async(object_id);" in start_helper
    assert "object_store_set_active(object_id)" not in start_helper
    assert "measurement_service_set_start_object_callback" in header
    assert "web_server_measurement_select_object" in web_server


def test_cache_refresh_and_measurement_start_share_reservation():
    arbitration = read("components/web_server/web_status_cache_arbitration.c")
    arbitration_header = read("components/web_server/web_status_cache_arbitration.h")
    measurement = read("components/measurement_service/measurement_service.c")
    web_server = read("components/web_server/web_server.c")

    assert "try_reserve_refresh" in arbitration_header
    assert "try_reserve_start" in arbitration_header
    assert "!arbitration->start_reserved" in arbitration
    assert "arbitration->start_reserved = true" in arbitration
    assert "measurement_service_set_start_reservation_release_callback" in measurement
    assert "measurement_service_release_start_reservation();" in measurement
    assert "web_status_json_objects_release_start_reservation" in web_server


def test_legacy_recording_aliases_use_measurement_service_and_cache_is_fail_safe():
    control = read("components/web_server/web_api_control.c")
    arbitration = read("components/web_server/web_status_cache_arbitration.c")
    status_objects = read("components/web_server/web_status_json_objects.c")

    assert "web_control_measurement_start" in control
    assert 'strcmp(action, "recording_start") == 0 ||' in control
    assert 'strcmp(action, "recording_stop") == 0 ||' in control
    assert "return measurement_service_start_object_async(object_id);" in control
    assert "return measurement_service_stop_async();" in control
    assert "return seismic_recorder_start(object_id);" not in control
    assert 'return seismic_recorder_stop();' not in control

    start_reservation = arbitration[arbitration.index(
        "bool web_status_cache_arbitration_try_reserve_start"
    ):]
    assert "const bool allowed = !arbitration->cache_refreshing &&" in start_reservation
    assert "arbitration->cache_ready &&" not in start_reservation
    assert "if (s_arbitration.mutex == NULL)" in status_objects
    assert "there is no refresh operation that can race local TFT recording" in status_objects


def test_acquisition_stop_cannot_bypass_measurement_service():
    control = read("components/web_server/web_api_control.c")
    header = read("components/measurement_service/include/measurement_service.h")
    acquisition_stop = control[
        control.index("static esp_err_t web_control_acquisition_stop"):
        control.index("static esp_err_t web_control_process")
    ]

    assert "measurement_service_snapshot_blocks_acquisition_stop" in header
    assert "measurement_service_get_snapshot(&measurement)" in acquisition_stop
    assert "measurement_service_snapshot_blocks_acquisition_stop(&measurement)" in acquisition_stop
    assert "return ESP_ERR_INVALID_STATE;" in acquisition_stop
    assert "seismic_recorder_stop" not in acquisition_stop
    assert "seismic_recorder_wait_for_stop" not in acquisition_stop
    assert "seismic_acquisition_stop();" in acquisition_stop


def test_webui_uses_measurement_state_to_lock_acquisition_controls():
    source = read("webui/src/features/objects/ObjectsFeature.tsx")
    transition = read("webui/src/features/objects/measurementTransition.ts")

    selected_recording = source[
        source.index("const selectedRecording"):
        source.index("const selectedSessions")
    ]
    assert "measurementOwnsRecorder(status)" in transition
    assert "measurementOwnsRecorder" in source
    assert "recording?.running === true" not in selected_recording
    assert "measurementLocked" in source
    assert "status?.acquisition.running !== true || measurementLocked" in source


def test_ui_header_does_not_reinvalidate_unchanged_labels():
    shell = read("components/ui/ui_shell.c")
    system = read("components/ui/ui_system.c")

    assert "ui_set_label_text_if_changed" in shell
    assert "ui_set_label_color_if_changed" in shell
    assert "lv_label_set_text(context->header_source" not in shell
    assert "lv_label_set_text(context->header_wifi" not in shell
    assert "ui_system_label_set_if_changed" in system


def test_tft_measurement_path_uses_envelope_arrays_without_point_updates():
    measurements = read("components/ui/ui_measurements.c")
    reducer = read("components/ui/ui_measurement_trace.c")
    kconfig = read("main/Kconfig.projbuild")

    assert "lv_chart_set_series_ext_y_array" in measurements
    assert "lv_chart_set_series_value_by_id" not in measurements
    assert "LV_PART_INDICATOR" in measurements
    assert "config UI_MEASUREMENTS_GRAPHS_ENABLED" in kconfig
    assert "UI_MEASUREMENT_CHART_PERIOD_US 125000U" in measurements
    assert "ui_measurements_create_graph_container_locked" in measurements
    assert "lv_obj_invalidate(context->measurement_graphs)" in measurements
    assert "lv_obj_invalidate(context->measurement_screen)" not in measurements
    assert "lv_chart_refresh(chart)" not in measurements
    assert "DIGITAL_INTERFERENCE" in measurements
    assert "DATA? DIGITAL" in measurements
    assert "s_last_digital_interference" in measurements
    assert "digital_interference == s_last_digital_interference" in measurements
    assert "channel_mean" in reducer
    assert "trace_min" in reducer
    assert "trace_max" in reducer


def test_non_realtime_storage_census_and_session_paths_are_heap_owned():
    profile = read("components/object_store/object_store_profile.c")
    recorder_fs = read("components/seismic_recorder/seismic_recorder_fs.c")
    recorder_json = read("components/seismic_recorder/seismic_recorder_json.c")
    atomic_write = read("components/storage_manager/storage_manager_text_write.c")

    assert "object_store_summary_t summaries[" not in profile
    assert "calloc(" in profile
    assert "free(summaries);" in profile
    assert "char sessions_path[SEISMIC_RECORDER_PATH_SIZE]" not in recorder_fs
    assert "char session_path[SEISMIC_RECORDER_PATH_SIZE]" not in recorder_fs
    assert "char path[SEISMIC_RECORDER_PATH_SIZE]" not in recorder_json
    assert "storage_manager_atomic_paths_t *paths" in atomic_write
    assert "storage_manager_atomic_paths_t paths" not in atomic_write


def test_measurement_snapshots_are_nonblocking_requests():
    source = read("components/measurement_service/measurement_service.c")

    assert "runtime_observability_request_snapshot(\"recording-active\")" in source
    assert "runtime_observability_request_snapshot(\"after-recorder-finalization\")" in source
    assert "runtime_observability_log_snapshot(\"recording-active\")" not in source
    assert "runtime_observability_log_snapshot(\"after-recorder-finalization\")" not in source


def test_psram_dma_audit_records_installed_idf_decision():
    audit = read("docs/psram-dma-audit.md")

    assert "esp_driver_spi/src/gpspi/spi_master.c" in audit
    assert "SPI_TRANS_DMA_USE_PSRAM" in audit
    assert "esp_driver_sdspi/src/sdspi_host.c" in audit
    assert "get_block_buf()" in audit
    assert "internal DMA bounce" in audit
