"""Source contracts for cooperative object/profile listing.

These checks prove lock/window structure only. They do not claim physical SD,
SPI2, FATFS, LVGL, or scheduler timing; that remains live-board HIL evidence.
"""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RESPONSIVE = (
    REPO_ROOT / "components" / "object_store" / "object_store_responsive.c"
).read_text(encoding="utf-8")
OPERATIONS = (
    REPO_ROOT / "components" / "object_store" / "object_store_operations.c"
).read_text(encoding="utf-8")


def position(source: str, needle: str) -> int:
    value = source.find(needle)
    assert value >= 0, f"missing source contract marker: {needle}"
    return value


def test_responsive_listing_does_not_delegate_to_monolithic_index_build():
    assert "object_store_list_profiles_with_defaults(" not in RESPONSIVE
    assert "object_store_fs_list(" in RESPONSIVE
    assert "object_store_get_profile(" in RESPONSIVE
    assert "object_store_get_cable_defaults(" in RESPONSIVE


def test_census_guard_is_closed_before_per_object_profile_reads():
    """Compare executable call markers, not function names mentioned in comments."""
    begin = position(
        RESPONSIVE,
        "esp_err_t err = object_store_internal_begin_file_operation();",
    )
    census = position(RESPONSIVE, "err = object_store_fs_list(")
    end = position(RESPONSIVE, "object_store_internal_end_file_operation();")
    per_object = position(RESPONSIVE, "err = object_store_get_profile(")
    assert begin < census < end < per_object


def test_profile_walk_yields_between_bounded_metadata_windows():
    assert "taskYIELD()" in RESPONSIVE
    assert "PROFILE_SCAN mode=cooperative" in RESPONSIVE


def test_object_service_list_uses_cooperative_path_too():
    assert "object_store_list_profiles_responsive(" in OPERATIONS
    assert "object_store_list_profiles(" not in OPERATIONS
