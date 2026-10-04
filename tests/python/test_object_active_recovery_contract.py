from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_slice(source: str, name: str, next_name: str | None = None) -> str:
    start = source.index(name)
    if next_name is None:
        return source[start:]
    end = source.index(next_name, start + len(name))
    return source[start:end]


def test_shared_metadata_identity_contract_is_used_by_store_and_recovery() -> None:
    fmt = read("components/object_format/object_format.c")
    recovery = read("components/object_recovery/object_recovery_metadata_validate.c")
    profile = read("components/object_store/object_store_profile.c")

    helper = "object_format_metadata_identity_is_valid"
    assert helper in fmt
    assert helper in recovery
    assert helper in profile


def test_set_active_validates_before_persistent_and_runtime_commit() -> None:
    source = read("components/object_store/object_store_mutations.c")
    block = function_slice(source, "esp_err_t object_store_set_active", "esp_err_t object_store_delete")

    validate = block.index("object_store_json_read_name_locked")
    persistent = block.index("object_store_fs_save_active")
    runtime = block.index("object_store_internal_set_active_id")
    cache = block.index("object_store_profile_cache_note_active_locked")
    assert validate < persistent < runtime < cache


def test_background_recovery_guards_active_at_destructive_boundary() -> None:
    scan = read("components/object_recovery/object_recovery_scan.c")
    guard = function_slice(
        scan,
        "static esp_err_t object_recovery_check_one_with_policy",
        "esp_err_t object_recovery_check_one("
    )
    assert guard.index("object_recovery_background_delete_guard") < guard.index("storage_manager_remove_tree")

    recovery = read("components/object_recovery/object_recovery.c")
    assert "object_recovery_check_one_background" in recovery
    active = function_slice(recovery, "esp_err_t object_recovery_run_active", None)
    assert "false" in active


def test_recovery_completion_invalidates_store_before_web_refresh() -> None:
    startup = read("main/app_startup.c")
    block = function_slice(
        startup,
        "static void app_startup_background_recovery_complete",
        "static void app_startup_recorder_recovery_requested"
    )
    invalidate = block.index("object_store_invalidate_profile_cache")
    refresh = block.index("web_server_enable_status_cache_refresh")
    assert invalidate < refresh


def test_tft_recovery_completion_clears_copied_object_list_without_rescan() -> None:
    source = read("components/ui/ui_navigation.c")
    start = source.index("case APP_EVENT_STORAGE_RECOVERY_FINISHED:")
    end = source.index("case APP_EVENT_KEYPAD:", start)
    block = source[start:end]

    assert "context->objects_loaded = false" in block
    assert "context->object_count = 0U" in block
    assert "context->object_index = 0U" in block
    assert "context->objects_mode = UI_OBJECTS_MENU" in block
    assert "object_service_list_async" not in block


def test_objects_screen_never_starts_archive_listing_during_recovery() -> None:
    registry = read("components/ui/ui_screen_registry.c")
    opening = function_slice(
        registry,
        "static void ui_screen_open_objects(void)",
        "static void ui_screen_open_measurements(void)",
    )
    assert opening.index("context->storage_recovery_active") < opening.index(
        "object_service_list_async"
    )
    assert "UI_I18N_KEY_SHARED_CHECKING_STORAGE" in opening

    navigation = read("components/ui/ui_navigation.c")
    started = navigation.index("case APP_EVENT_STORAGE_RECOVERY_STARTED:")
    finished = navigation.index("case APP_EVENT_STORAGE_RECOVERY_FINISHED:", started)
    start_block = navigation[started:finished]
    assert "UI_SCREEN_OBJECTS" in start_block
    assert "UI_I18N_KEY_SHARED_CHECKING_STORAGE" in start_block
    assert "ui_render_objects_locked" in start_block

    objects = read("components/ui/ui_objects_input.c")
    input_block = function_slice(
        objects,
        "void ui_handle_objects_key_locked(",
        None,
    )
    assert input_block.index("context->storage_recovery_active") < input_block.index(
        "object_service_is_busy"
    )


def test_get_active_remains_ram_only() -> None:
    source = read("components/object_store/object_store.c")
    block = function_slice(
        source,
        "esp_err_t object_store_get_active",
        "esp_err_t object_store_invalidate_profile_cache"
    )
    assert "storage_manager_" not in block
    assert "object_store_fs_" not in block


def test_corrupt_profiles_are_omittable_but_real_errors_remain_visible() -> None:
    profile = read("components/object_store/object_store_profile.c")
    responsive = read("components/object_store/object_store_responsive.c")
    assert "ESP_ERR_INVALID_RESPONSE" in profile
    assert "object_store_profile_error_is_omittable" in profile
    assert "err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_RESPONSE" in responsive


def test_prepare_metadata_uses_two_path_buffers_not_three() -> None:
    source = read("components/object_recovery/object_recovery_metadata.c")
    block = function_slice(
        source,
        "esp_err_t object_recovery_fs_prepare_metadata",
        None,
    )

    # The two path buffers live in one heap block (2 x PATH_SIZE) so the
    # recovery task stack is not spent on path storage.
    assert "malloc(\n        2U * OBJECT_RECOVERY_PATH_SIZE" in block
    assert "char *primary = paths;" in block
    assert "char *sidecar = paths + OBJECT_RECOVERY_PATH_SIZE;" in block
    assert "char backup[" not in block
    assert "char temporary[" not in block
    assert "char primary[" not in block
    assert "char sidecar[" not in block

    # The sidecar buffer must be rebuilt for both sidecar file names, never
    # both live at once.
    assert block.count('"object.json.bak"') == 2
    assert block.count('"object.json.tmp"') == 2


def test_main_stack_hwm_telemetry_follows_synchronous_startup() -> None:
    startup = read("main/app_startup.c")
    block = function_slice(startup, "void app_startup_run(void)", None)

    startup_total = block.index('"startup_total"')
    hwm_call = block.index("app_startup_log_stack_hwm()")
    assert startup_total < hwm_call

    assert 'STACK_HWM task=main free=%u target=%u' in startup
    assert "uxTaskGetStackHighWaterMark(NULL)" in startup


def test_object_recovery_stack_hwm_telemetry_fires_on_every_worker_exit() -> None:
    worker = read("components/object_recovery/object_recovery_worker.c")
    block = function_slice(
        worker,
        "static void object_recovery_background_task(void *argument)",
        "esp_err_t object_recovery_start_background_with_callback(",
    )

    complete_call = block.index("complete_fn(err, complete_context);")
    hwm_call = block.index("object_recovery_log_stack_hwm();")
    retire = block.index("s_task = NULL;")
    delete_call = block.index("vTaskDeleteWithCaps(NULL);")

    # Emitted after the completion callback and before task retirement, on
    # the single shared tail reached by every exit path (normal pass
    # completion, error exit, and stop-requested exit all break into it).
    assert complete_call < hwm_call < retire < delete_call

    assert 'STACK_HWM task=object_recovery free=%u target=%u' in worker
    assert "uxTaskGetStackHighWaterMark(NULL)" in worker


def test_max73_create_commit_order_is_unchanged() -> None:
    source = read("components/object_store/object_store_operations.c")
    block = function_slice(
        source,
        "static esp_err_t object_store_create_transaction",
        "esp_err_t object_store_create("
    )
    metadata = block.index("object_store_json_write_metadata")
    active = block.index("object_store_fs_save_active")
    runtime = block.index("object_store_internal_set_active_id")
    cache = block.index("object_store_profile_cache_note_create_locked")
    assert metadata < active < runtime < cache
