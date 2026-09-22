"""Source contracts for the MAX-168 offline-analysis infrastructure slice:
raw per-session sample read, frozen per-session geometry read-back, and
localization run listing."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_raw_session_read_is_public_and_bypasses_cache_and_lod():
    header = read("components/seismic_recorder/include/seismic_recorder.h")
    reader = read("components/seismic_recorder/seismic_recorder_reader.c")

    assert "seismic_recorder_read_session_raw(" in header
    body_start = reader.index("esp_err_t seismic_recorder_read_session_raw(")
    body_end = reader.index("\n}\n", body_start)
    body = reader[body_start:body_end]

    assert "seismic_recorder_cache_contains_session_range(" not in body
    assert "seismic_recorder_reader_try_lod_path(" not in body
    assert "seismic_recorder_waveform_read_raw_source(" in body


def test_session_geometry_read_is_never_the_object_live_geometry():
    header = read("components/seismic_recorder/include/seismic_recorder.h")
    source = read("components/seismic_recorder/seismic_recorder_json.c")

    assert "seismic_recorder_read_session_geometry(" in header
    assert "object_store_get_profile(" not in source[source.index(
        "esp_err_t seismic_recorder_read_session_geometry("
    ):]


def test_run_store_never_overwrites_and_skips_bad_files_when_listing():
    source = read("components/localization_run_store/localization_run_store.c")
    header = read("components/localization_run_store/include/localization_run_store.h")

    assert "localization_run_store_list(" in header
    list_start = source.index("esp_err_t localization_run_store_list(")
    list_body = source[list_start:]
    assert "continue;" in list_body
    # The listing loop must never call remove()/unlink() -- it only reads.
    assert "remove(" not in list_body
    assert "unlink(" not in list_body
