from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_slice(source: str, start: str, end: str | None = None) -> str:
    start_index = source.index(start)
    if end is None:
        return source[start_index:]
    end_index = source.index(end, start_index + len(start))
    return source[start_index:end_index]


def test_object_store_lease_is_authoritative_delete_guard() -> None:
    header = read("components/object_store/include/object_store.h")
    assert "object_store_lease_acquire" in header
    assert "object_store_lease_release" in header
    assert "OBJECT_STORE_MAX_LEASES 4U" in header

    mutations = read("components/object_store/object_store_mutations.c")
    delete = function_slice(mutations, "esp_err_t object_store_delete(")
    lease_check = delete.index("object_store_lease_is_object_held_locked")
    assert lease_check < delete.index("object_store_fs_clear_active")
    assert lease_check < delete.index("object_store_internal_clear_active_id")
    assert lease_check < delete.index("object_store_fs_delete_object")
    assert lease_check < delete.index("object_store_profile_cache_note_delete_locked")

    lease = read("components/object_store/object_store_lease.c")
    acquire = function_slice(
        lease,
        "esp_err_t object_store_lease_acquire(",
        "esp_err_t object_store_lease_release("
    )
    assert acquire.index("object_store_internal_begin_file_operation") < acquire.index("object_store_fs_object_exists")
    release = function_slice(
        lease,
        "esp_err_t object_store_lease_release(",
        "esp_err_t object_store_object_is_leased("
    )
    assert "object_store_internal_begin_state_operation" in release
    assert "object_store_internal_begin_file_operation" not in release


def test_background_recovery_checks_active_and_lease_before_remove() -> None:
    recovery = read("components/object_recovery/object_recovery_scan.c")
    guarded = function_slice(
        recovery,
        "static esp_err_t object_recovery_check_one_with_policy(",
        "esp_err_t object_recovery_check_one("
    )
    active = guarded.index("object_recovery_background_delete_guard")
    lease = guarded.index("object_recovery_background_lease_guard")
    remove = guarded.index("storage_manager_remove_tree")
    assert active < lease < remove

    cmake = read("components/object_recovery/CMakeLists.txt")
    assert "object_store" in cmake


def test_recorder_lifetime_wraps_full_start_and_terminal_metadata() -> None:
    cmake = read("components/seismic_recorder/CMakeLists.txt")
    assert "seismic_recorder_object_lifetime.c" in cmake
    assert "seismic_recorder_start=seismic_recorder_start_unleased" in cmake
    assert "seismic_recorder_json_write_metadata=seismic_recorder_lifetime_write_metadata" in cmake

    lifetime = read("components/seismic_recorder/seismic_recorder_object_lifetime.c")
    start = function_slice(
        lifetime,
        "esp_err_t seismic_recorder_start(",
        "esp_err_t seismic_recorder_lifetime_write_metadata("
    )
    assert start.index("object_store_lease_acquire") < start.index("seismic_recorder_start_unleased")
    assert start.index("seismic_recorder_start_unleased") < start.index("seismic_recorder_lifetime_release")
    assert "if (err == ESP_OK)" in start

    terminal = function_slice(
        lifetime,
        "esp_err_t seismic_recorder_lifetime_write_metadata("
    )
    assert terminal.index("seismic_recorder_json_write_metadata") < terminal.index("seismic_recorder_lifetime_release")
    assert 'strcmp(status, "complete")' in terminal
    assert 'strcmp(status, "error")' in terminal

    recorder = read("components/seismic_recorder/seismic_recorder.c")
    original_start = function_slice(
        recorder,
        "esp_err_t seismic_recorder_start(",
        "esp_err_t seismic_recorder_stop("
    )
    assert "object_store_get_profile" in original_start
    stop_step = function_slice(
        recorder,
        "static void seismic_recorder_stop_step(",
        "static void seismic_recorder_flush_if_due("
    )
    metadata_stage = function_slice(
        stop_step,
        "case SEISMIC_RECORDER_STOP_STAGE_METADATA:",
        "case SEISMIC_RECORDER_STOP_STAGE_NONE:"
    )
    assert metadata_stage.index("seismic_recorder_json_write_metadata") < metadata_stage.index("s_snapshot.running = false")
    assert metadata_stage.index("s_snapshot.running = false") < metadata_stage.index("xEventGroupSetBits")


def test_scan_lease_wraps_entire_save_load_handler() -> None:
    control = read("components/web_server/web_api_control.c")
    wrapper = function_slice(
        control,
        "static esp_err_t web_control_scan_try_handle(",
        "static const char *web_control_string("
    )
    fallback = wrapper.index("return web_scan_store_try_handle(request, handled)")
    acquire = wrapper.index("object_store_lease_acquire")
    leased_scan = wrapper.index("web_scan_store_try_handle(request, handled)", acquire)
    release = wrapper.index("object_store_lease_release", leased_scan)
    assert fallback < acquire < leased_scan < release

    handler = function_slice(
        control,
        "static esp_err_t web_api_control_handler(",
        "esp_err_t web_api_control_register("
    )
    assert "web_control_scan_try_handle" in handler
    assert "web_scan_store_try_handle(request, &scan_handled)" not in handler

    scan = read("components/web_server/web_scan_store.c")
    save = function_slice(scan, "static esp_err_t web_scan_save(", "static esp_err_t web_scan_open_for_stream(")
    load = function_slice(scan, "static esp_err_t web_scan_load(", "static esp_err_t web_scan_delete(")
    assert "httpd_req_recv" in save
    assert "storage_manager_operation_begin" not in save
    assert "httpd_resp_send_chunk" in load
    assert "storage_manager_operation_begin" not in load
