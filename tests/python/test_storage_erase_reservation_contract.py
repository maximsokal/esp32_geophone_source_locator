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


def test_destructive_reservation_and_lease_acquire_are_atomic_peers() -> None:
    lease = read("components/object_store/object_store_lease.c")
    acquire = function_slice(
        lease,
        "esp_err_t object_store_lease_acquire(",
        "esp_err_t object_store_lease_release("
    )
    assert acquire.index("object_store_internal_begin_file_operation") < acquire.index("s_destructive_token_id")
    assert acquire.index("s_destructive_token_id") < acquire.index("object_store_fs_object_exists")

    reserve = function_slice(
        lease,
        "esp_err_t object_store_destructive_reservation_acquire(",
        "esp_err_t object_store_destructive_reservation_release("
    )
    assert "object_store_internal_begin_state_operation" in reserve
    assert "object_store_any_lease_held_locked" in reserve
    assert "s_destructive_token_id" in reserve
    assert "storage_manager" not in reserve


def test_storage_erase_owns_reservation_across_destructive_transaction() -> None:
    operations = read("components/storage_service/storage_service_operations.c")
    erase = function_slice(operations, "bool storage_service_process_erase(")
    reserve = erase.index("object_store_destructive_reservation_acquire")
    storage_begin = erase.index("storage_manager_operation_begin")
    destructive = erase.index("storage_manager_erase_application_data")
    reset = erase.index("object_store_reset")
    storage_end = erase.index("storage_manager_operation_end")
    release = erase.index("object_store_destructive_reservation_release")
    shutdown = erase.index("power_control_is_shutdown_requested")
    assert reserve < storage_begin < destructive < reset < storage_end < release < shutdown


def test_successful_object_store_reset_drops_storage_derived_cache_only() -> None:
    store = read("components/object_store/object_store.c")
    reset = function_slice(store, "esp_err_t object_store_reset(")
    assert "object_store_internal_clear_active_id" in reset
    assert "object_store_profile_cache_reset" in reset
    assert "object_store_profile_cache_note_active_locked" not in reset
    assert "object_store_lease_reset_locked" not in reset


def test_storage_operations_host_contract_is_wired_in_coverage_manifest() -> None:
    manifest = read("tests/host/manifest.d/coverage-services.json")
    assert '"storage_service_operations"' in manifest
    assert '"components/storage_service/storage_service_operations.c"' in manifest
    assert '"tests/host/storage_service_operations_test.c"' in manifest
