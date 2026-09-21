"""Source contracts for freezing per-recording geometry into session.json (MAX-166)."""

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


def test_profile_is_fetched_after_the_geometry_solve_not_before() -> None:
    """A recording's frozen session metadata must reflect the geometry this
    exact recording solved, not a stale pre-solve fetch of the object's
    previous geometry. seismic_recorder_prepare_geometry() is what calls
    object_store_set_geometry() -- object_store_get_profile() must run
    strictly after it returns."""
    # seismic_recorder.c's own literal source says "seismic_recorder_start";
    # the object-lifetime lease wrapper renames it to
    # "seismic_recorder_start_unleased" only via a build-time
    # set_source_files_properties() macro (see seismic_recorder/CMakeLists.txt),
    # so match the source's actual spelling here.
    recorder = read("components/seismic_recorder/seismic_recorder.c")
    start = function_slice(
        recorder,
        "esp_err_t seismic_recorder_start(",
        "esp_err_t seismic_recorder_stop(",
    )

    prepare_geometry_call = start.index("seismic_recorder_prepare_geometry(object_id)")
    profile_fetch_call = start.index("object_store_get_profile(object_id, profile)")

    assert prepare_geometry_call < profile_fetch_call


def test_session_json_serializes_the_frozen_geometry() -> None:
    source = read("components/seismic_recorder/seismic_recorder_json.c")

    assert "seismic_recorder_json_add_geometry(" in source
    write_metadata = function_slice(
        source, "esp_err_t seismic_recorder_json_write_metadata("
    )
    cables_call = write_metadata.index("seismic_recorder_json_add_cables(root, profile)")
    geometry_call = write_metadata.index(
        "seismic_recorder_json_add_geometry(root, &profile->geometry)"
    )
    assert cables_call < geometry_call

    add_geometry = function_slice(
        source,
        "static bool seismic_recorder_json_add_geometry(",
        "static bool seismic_recorder_json_add_cadence(",
    )
    assert '"anchored"' in add_geometry
    assert '"pods"' in add_geometry
    assert '"computed"' in add_geometry
    assert '"leveled"' in add_geometry
    assert '"x_mm"' in add_geometry
    assert '"y_mm"' in add_geometry
    assert '"z_mm"' in add_geometry
    assert '"computed_at_ms"' in add_geometry
