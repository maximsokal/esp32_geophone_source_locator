from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def _create_block(source: str) -> str:
    start = source.index('if (strcmp(action, "create_object") == 0)')
    end = source.index('const char *object_id = web_control_string', start)
    return source[start:end]


def test_web_create_is_one_transactional_store_call():
    source = _read("components/web_server/web_api_control.c")
    block = _create_block(source)

    assert "object_store_create_named(name, &created)" in block
    assert "object_store_create(&created)" not in block
    assert "object_store_rename(" not in block
    assert "object_store_delete(" not in block
    assert "web_control_name_valid" not in source


def test_object_store_create_keeps_active_as_final_persistent_commit():
    source = _read("components/object_store/object_store_operations.c")
    transaction = source[source.index("static esp_err_t object_store_create_transaction"):]

    metadata = transaction.index("object_store_json_write_metadata")
    active = transaction.index("object_store_fs_save_active")
    runtime = transaction.index("object_store_internal_set_active_id")
    cache = transaction.index("object_store_profile_cache_note_create_locked")

    assert metadata < active < runtime < cache
    assert "object_store_fs_delete_object(object_id)" in transaction
    assert "object_store_profile_cache_reset()" in transaction
