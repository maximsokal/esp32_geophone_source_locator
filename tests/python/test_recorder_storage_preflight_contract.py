from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (REPO_ROOT / relative).read_text(encoding="utf-8")


def test_session_creation_checks_free_space_before_allocating_session_id_or_directory():
    source = read("components/seismic_recorder/seismic_recorder_fs.c")
    start = source.index("esp_err_t seismic_recorder_fs_create_session(")
    end = source.index("esp_err_t seismic_recorder_fs_write_frames(", start)
    create_session = source[start:end]

    outer_guard = create_session.index("storage_manager_operation_begin()")
    preflight = create_session.index("seismic_recorder_storage_preflight(")
    next_session = create_session.index("seismic_recorder_fs_next_session_id(")
    mkdir = create_session.index("storage_manager_create_directory_exclusive(")
    fopen = create_session.index('fopen(path, "wb")')
    outer_release = create_session.rindex("storage_manager_operation_end()")

    assert outer_guard < preflight < next_session < mkdir < fopen < outer_release


def test_preflight_query_is_balanced_and_uses_fatfs_free_space_result():
    source = read("components/seismic_recorder/seismic_recorder_storage_preflight.c")
    start = source.index("esp_err_t seismic_recorder_storage_preflight(")
    preflight = source[start:]

    begin = preflight.index("storage_manager_operation_begin()")
    query = preflight.index("sd_card_get_info(&info)")
    end = preflight.index("storage_manager_operation_end()")
    comparison = preflight.index("info.free_bytes < required_bytes")

    assert begin < query < end < comparison
    assert "return ESP_ERR_NO_MEM;" in preflight
    assert "RECORDER_PREFLIGHT result=query-failed" in preflight
    assert "RECORDER_PREFLIGHT result=insufficient-space" in preflight
    assert "RECORDER_PREFLIGHT result=pass" in preflight


def test_required_space_is_rate_aware_and_reserves_write_behind_capacity():
    header = read("components/seismic_recorder/seismic_recorder_storage_preflight.h")
    source = read("components/seismic_recorder/seismic_recorder_storage_preflight.c")

    assert "SEISMIC_RECORDER_START_RUNWAY_SECONDS  60U" in header
    assert "sample_rate_hz" in source
    assert "sizeof(seismic_frame_t)" in source
    assert "CONFIG_SEISMIC_RECORDER_PSRAM_CACHE_KB" in source
    assert "cache_reserve_bytes" in source
    assert "seismic_recorder_saturating_multiply" in source
    assert "seismic_recorder_saturating_add" in source


def test_recorder_component_builds_real_preflight_and_declares_sd_card_dependency():
    cmake = read("components/seismic_recorder/CMakeLists.txt")

    assert '"seismic_recorder_storage_preflight.c"' in cmake
    assert "sd_card" in cmake


def test_filesystem_host_contract_stubs_only_external_preflight_boundary():
    stub = read(
        "components/seismic_recorder/tests/stubs/seismic_recorder_storage_preflight.h"
    )
    manifest = read("tests/host/manifest.d/coverage-recorder.json")

    assert "static inline esp_err_t seismic_recorder_storage_preflight" in stub
    assert '"components/seismic_recorder/seismic_recorder_storage_preflight.c"' in manifest
    assert '"tests/host/seismic_recorder_storage_preflight_test.c"' in manifest
