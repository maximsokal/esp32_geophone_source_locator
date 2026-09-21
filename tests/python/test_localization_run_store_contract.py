"""Source contracts for crash-safe localization-run persistence (MAX-140)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_reuses_the_one_existing_atomic_write_primitive():
    """Must not reimplement temp-file/fsync/backup logic -- that already
    exists in storage_manager and every other JSON writer in this codebase
    reuses it as-is."""
    source = read("components/localization_run_store/localization_run_store.c")

    assert "storage_manager_atomic_write_text(" in source
    assert "fopen" not in source
    assert "fwrite" not in source
    assert "fsync" not in source
    assert "rename(" not in source


def test_follows_the_projects_existing_session_path_layout():
    source = read("components/localization_run_store/localization_run_store.c")
    assert 'STORAGE_MANAGER_OBJECTS_PATH' in source
    assert '"%s/%s/sessions/%s/' in source


def test_every_run_gets_its_own_file_not_a_fixed_latest_path():
    """MAX-138's 'never overwrite the individual outputs' and this ticket's
    own 'recalculating preserves the earlier result' both fall out of this:
    the filename must depend on the run's own identity, not be constant."""
    source = read("components/localization_run_store/localization_run_store.c")

    assert "record->kind" in source or "kind_slug(record->kind)" in source
    assert "record->method_name" in source
    assert "record->computed_at_ms" in source


def test_method_name_is_validated_before_it_reaches_a_path():
    """method_name is caller-supplied and flows directly into a filename --
    guard against path traversal via '..' or '/' in a crafted method_name."""
    source = read("components/localization_run_store/localization_run_store.c")
    assert "is_filename_safe" in source
    assert "isalnum(" in source


def test_reuses_the_existing_object_and_session_id_validators():
    source = read("components/localization_run_store/localization_run_store.c")
    assert "object_format_id_is_valid(" in source


def test_pure_json_serialization_stays_decoupled_from_file_io():
    """localization_run_record.h's own contract: it owns no file I/O. This
    is exactly why the file-writing layer lives in its OWN component
    (localization_run_store) rather than being folded into
    localization_results -- that separation is what keeps the existing
    'localization_results owns no file I/O' contract test meaningful."""
    header = read("components/localization_results/include/localization_run_record.h")
    results_cmake = read("components/localization_results/CMakeLists.txt")
    store_source = read("components/localization_run_store/localization_run_store.c")
    store_cmake = read("components/localization_run_store/CMakeLists.txt")

    assert "This component owns no file" in header
    assert "storage_manager" not in results_cmake
    assert "localization_run_record_to_json(" in store_source
    assert "storage_manager" in store_cmake
