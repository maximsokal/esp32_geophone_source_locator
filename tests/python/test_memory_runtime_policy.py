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
    assert '"sdkconfig.defaults;sdkconfig.storage.defaults;sdkconfig.lvgl.defaults;sdkconfig.tls.defaults;' in cmake
    assert "sdkconfig.button.defaults\"" in cmake

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

    # esp_lvgl_port owns the panel completion callback. Shared-SPI quiesce must
    # wait for all queued tx_color DMA work through a synchronous panel-IO
    # command instead of replacing that callback or spinning on LVGL internals.
    assert "ui_lvgl_drain_panel_io_locked" in tft
    assert "esp_lcd_panel_io_tx_param(" in tft
    assert "lvgl_port_lock(0)" in tft
    assert "lvgl_port_unlock()" in tft
    assert "esp_lcd_panel_io_register_event_callbacks(" not in tft
    assert "lv_display_set_flush_wait_cb(" not in tft
    assert "while (disp->flushing)" not in tft


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
    recorder_json = read("components/seismic_recorder/seismic_recorder_json.c")
    storage_header = read("components/storage_manager/include/storage_manager.h")
    storage_directory = read("components/storage_manager/storage_manager_directory.c")
    storage_tree = read("components/storage_manager/storage_manager_tree.c")
    storage_read = read("components/storage_manager/storage_manager_text_read.c")
    recording = read("components/web_server/web_status_json_recording.c")
    status_json = read("components/web_server/web_status_json.c")
    status_api = read("components/web_server/web_api_status.c")
    internal = read("components/web_server/web_server_internal.h")

    assert "seismic_recorder_list_sessions_responsive(" in objects
    assert "web_status_json_objects_refresh(void)" in objects
    assert "xTaskCreatePinnedToCoreWithCaps(" in objects
    assert "xQueueOverwrite(" in objects
    assert "object_service_event_observer_t" in read("components/object_service/include/object_service.h")
    assert "web_status_json_objects_object_event" in objects
    assert "WEB_OBJECT_CACHE_TTL_US" not in objects
    assert "web_status_json_objects_cache_ready" in internal
    assert "opendir(sessions_path)" in recorder_fs
    assert "readdir(directory)" in recorder_fs
    assert "seismic_recorder_fs_session_metadata_state" in recorder_fs
    assert "seismic_recorder_json_read_summary_known_present" in recorder_fs
    assert "storage_manager_read_text_known_present" in recorder_json
    assert "storage_manager_read_text_known_present" in storage_header
    assert "storage_manager_create_directory_exclusive" in storage_header
    assert "storage_manager_create_directory_exclusive" in storage_directory
    assert "session_directory_created" in recorder_fs
    assert "const esp_err_t cleanup_err" in recorder_fs
    assert "Failed to clean incomplete session" in recorder_fs
    assert "cleanup_err != ESP_ERR_NOT_FOUND" in recorder_fs
    assert "const int read_errno = errno" in storage_tree
    assert "const int close_result = closedir(directory)" in storage_tree
    assert "Failed to read directory" in storage_tree
    assert "bool is_root" in storage_tree
    assert "return is_root" in storage_tree
    assert "recover_backup" in storage_read
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
    envelope = read("components/seismic_buffer/seismic_envelope.c")
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
    assert "UI_I18N_KEY_MEASUREMENT_DIGITAL_FAULT" in measurements
    assert "s_last_digital_interference" in measurements
    assert "digital_interference == s_last_digital_interference" in measurements
    assert "channel_sum" in envelope
    assert "bucket_min" in envelope
    assert "bucket_max" in envelope
    assert "frames_consumed" in envelope
    assert "UI_MEASUREMENT_PREVIEW_CHUNK_FRAMES 1024U" in measurements
    assert "ui_measurement_trace_accumulator_add" in measurements
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


def test_network_lifecycle_does_not_run_heavy_work_in_sys_evt():
    web_server = read("components/web_server/web_server.c")
    wifi_manager = read("components/wifi_manager/wifi_manager.c")
    wifi_sta_events = read("components/wifi_manager/wifi_manager_sta_events.c")
    wifi_mode = read("components/wifi_manager/wifi_manager_mode.c")
    wifi_header = read("components/wifi_manager/include/wifi_manager.h")
    task_config = read("components/app_config/include/app_task_config.h")

    callback = web_server[
        web_server.index("static void web_server_network_event_handler("):
        web_server.index("static void web_server_https_event_handler(")
    ]

    assert "web_server_lifecycle_set_network_event_state" in callback
    assert "web_server_https_start_internal(" not in callback
    assert "web_server_https_stop_internal(" not in callback
    assert "web_server_http_start_internal(" not in callback

    assert "runtime_observability_request_snapshot(\"before-https\")" in web_server
    assert "runtime_observability_request_snapshot(\"after-https\")" in web_server
    assert "runtime_observability_log_snapshot(\"before-https\")" not in web_server
    assert "runtime_observability_log_snapshot(\"after-https\")" not in web_server
    assert "xQueueOverwrite(" in web_server
    assert "xQueueCreate(1U, sizeof(web_lifecycle_command_t))" in web_server
    assert "web_lifecycle_state_t" in web_server
    assert "WEB_LIFECYCLE_STATE_SETUP_AP" in web_server
    assert "WEB_LIFECYCLE_STATE_QUIESCED" in web_server
    assert "WEB_LIFECYCLE_STATE_STOPPED" in web_server
    assert "web_lifecycle_state_actions" in web_server
    assert "WEB_LIFECYCLE_COMMAND_" not in web_server
    assert "wifi_manager_mdns_stop();" in web_server
    assert "web_server_lifecycle_quiesce" in web_server
    assert "web_server_lifecycle_radio_stop_complete" in web_server
    assert "wifi_manager_set_radio_stop_lifecycle_callbacks" in web_server
    assert "web_server_mode_transition_complete" in web_server
    assert "wifi_manager_set_mode_transition_complete_callback" in web_server
    assert "web_server_lifecycle_set_authoritative_state" in web_server
    assert "xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY)" in web_server
    assert "wifi_manager_is_radio_state_uncertain()" in web_server
    assert "cleanup_result != ESP_OK" in web_server
    assert "radio state uncertain" in web_server
    assert "web_server_lifecycle_set_authoritative_state(desired_state)" in web_server
    network_enqueue_start = web_server.index(
        "static esp_err_t web_server_lifecycle_set_network_event_state("
    )
    network_enqueue = web_server[
        network_enqueue_start:
        web_server.index("static void web_server_mode_transition_complete(", network_enqueue_start)
    ]
    assert "xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY)" in network_enqueue
    assert "wifi_manager_is_radio_state_uncertain()" in network_enqueue
    assert "web_lifecycle_effective_network_state" in network_enqueue
    assert "web_server_lifecycle_set_desired_state_locked(desired_state)" in network_enqueue
    assert network_enqueue.index("wifi_manager_is_radio_state_uncertain()") < network_enqueue.index(
        "web_server_lifecycle_set_desired_state_locked(desired_state)"
    )
    assert "running && setup_mode" in web_server
    assert "wifi_manager_sta_ip_string" in web_server
    assert "s_lifecycle_quiesce_pending" in web_server
    assert "s_lifecycle_transition_active" in web_server
    assert "s_lifecycle_deferred_state_valid" in web_server
    assert "s_lifecycle_stable_state" in web_server
    assert "s_lifecycle_rollback_state" in web_server
    assert "s_lifecycle_rollback_state = s_lifecycle_stable_state" in web_server
    assert "web_server_lifecycle_reconcile_failed_transition_locked" in web_server
    ack = web_server[
        web_server.index("static void web_server_lifecycle_ack_quiesce(esp_err_t result)\n{"):
        web_server.index(
            "static void web_server_lifecycle_radio_stop_complete(\n"
            "    void *context,\n"
            "    esp_err_t radio_stop_result\n"
            ")\n{"
        )
    ]
    assert "s_lifecycle_quiesce_pending = false" in ack
    assert "s_lifecycle_transition_active = false" not in ack
    complete = web_server[
        web_server.index(
            "static void web_server_lifecycle_radio_stop_complete(\n"
            "    void *context,\n"
            "    esp_err_t radio_stop_result\n"
            ")\n{"
        ):
        web_server.index("static void web_server_lifecycle_unregister_wifi_hook(")
    ]
    assert "s_lifecycle_transition_active = false" in complete
    assert "web_server_lifecycle_reconcile_deferred_locked" in complete
    assert "web_server_lifecycle_reconcile_failed_transition_locked" in complete
    assert "radio_stop_result == ESP_OK" in complete
    assert "xTaskCreatePinnedToCoreWithCaps(" in web_server
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in web_server
    assert '"web_lifecycle"' in web_server
    assert "APP_TASK_STACK_WEB_LIFECYCLE" in web_server
    assert "APP_TASK_PRIORITY_WEB_LIFECYCLE" in web_server

    assert "wifi_manager_mdns_init(" not in wifi_sta_events
    assert "wifi_manager_mdns_is_ready(" not in wifi_sta_events
    assert "wifi_manager_mdns_stop(" not in wifi_mode
    assert "wifi_manager_set_radio_stop_lifecycle_callbacks" in wifi_mode
    assert "wifi_manager_set_mode_transition_complete_callback" in wifi_mode
    assert "wifi_manager_abort_mode_transition" in wifi_mode
    assert "wifi_manager_recover_uncertain_radio_state" in wifi_mode
    assert "wifi_manager_internal_commit_state" in wifi_mode
    assert "state.radio_state_uncertain" in wifi_mode
    assert "wifi_manager_notify_mode_transition_complete" in wifi_mode
    assert "cleanup_result" in wifi_mode
    assert "wifi_manager_radio_stop_complete_callback_t" in wifi_mode
    assert "s_radio_stop_lifecycle_in_flight" in wifi_mode
    assert "const esp_err_t radio_stop_result" in wifi_mode
    assert "complete_callback(context, radio_stop_result)" in wifi_mode
    radio_stop = wifi_mode[
        wifi_mode.index("static esp_err_t wifi_manager_stop_radio("):
        wifi_mode.index("esp_err_t wifi_manager_enter_setup_mode(")
    ]
    assert "wifi_manager_prepare_radio_stop(" in radio_stop
    assert radio_stop.index("wifi_manager_prepare_radio_stop(") < radio_stop.index("esp_wifi_stop()")
    assert radio_stop.index("esp_wifi_stop()") < radio_stop.index("complete_callback(context, radio_stop_result)")
    assert "err == ESP_ERR_WIFI_NOT_STARTED" in radio_stop
    assert "? ESP_OK" in radio_stop
    for transition, next_transition in (
        ("wifi_manager_enter_setup_mode", "wifi_manager_connect_saved"),
        ("wifi_manager_connect_saved", "wifi_manager_stop"),
    ):
        start = wifi_mode.index(f"esp_err_t {transition}(")
        end = wifi_mode.index(f"\nesp_err_t {next_transition}(", start)
        body = wifi_mode[start:end]
        assert "wifi_manager_internal_commit_state(" in body
        assert "err = wifi_manager_abort_mode_transition(err);" in body
        assert body.index("wifi_manager_internal_commit_state(") < body.index(
            "wifi_manager_abort_mode_transition(err)"
        )
        assert body.index("wifi_manager_abort_mode_transition(err)") < body.rindex(
            "wifi_manager_internal_end_transition()"
        )
    assert "wifi_manager_mdns_init(void)" in wifi_header
    assert "wifi_manager_mdns_stop(void)" in wifi_header
    assert "wifi_manager_mdns_is_ready(void)" in wifi_header
    assert "wifi_manager_is_radio_state_uncertain(void)" in wifi_header
    assert "esp_err_t cleanup_result" in wifi_header
    assert "#include \"freertos/portmacro.h\"" in wifi_manager
    assert "static wifi_manager_internal_state_t s_state = { 0 };" in wifi_manager
    assert "static portMUX_TYPE s_state_lock" in wifi_manager
    assert "portENTER_CRITICAL(&s_state_lock)" in wifi_manager
    assert "portEXIT_CRITICAL(&s_state_lock)" in wifi_manager
    assert "s_state.running = running;" in wifi_manager
    assert "s_state.setup_mode = running && setup_mode;" in wifi_manager
    assert "s_state.radio_state_uncertain = radio_state_uncertain;" in wifi_manager
    assert "if (s_radio_state_uncertain)" not in wifi_manager
    assert "return s_radio_state_uncertain;" not in wifi_manager
    assert "APP_TASK_STACK_WEB_LIFECYCLE 8192" in task_config

    lifecycle_policy = read("components/web_server/web_server_lifecycle_policy.c")
    assert "web_lifecycle_effective_network_state" in lifecycle_policy
    assert "WEB_LIFECYCLE_ACTION_MDNS_STOP" in lifecycle_policy
    assert "WEB_LIFECYCLE_STATE_QUIESCED" in lifecycle_policy
    assert "WEB_LIFECYCLE_ACTION_EXIT" in lifecycle_policy


def test_psram_dma_audit_records_installed_idf_decision():
    audit = read("docs/psram-dma-audit.md")

    assert "esp_driver_spi/src/gpspi/spi_master.c" in audit
    assert "SPI_TRANS_DMA_USE_PSRAM" in audit
    assert "esp_driver_sdspi/src/sdspi_host.c" in audit
    assert "get_block_buf()" in audit
    assert "internal DMA bounce" in audit
