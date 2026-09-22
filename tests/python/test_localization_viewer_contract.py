"""Source contracts for MAX-148's first slice: exposing this session's own
frozen sensor geometry (X/Y/Z, leveled) alongside its persisted localization
runs, best-effort and never as a hard failure of the listing route."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def handler_source() -> str:
    return read("components/web_server/web_localization_api.c")


def test_geometry_read_never_uses_object_live_topology():
    source = handler_source()
    # Must reuse the same session-scoped reader localization_calculate_run()
    # already uses -- never the object's current live geometry.
    assert "seismic_recorder_read_session_geometry(object_id, session_id, &geometry)" in source


def test_geometry_read_failure_does_not_fail_the_request():
    source = handler_source()

    # The real call site (not the header doc comment mentioning the same
    # function name earlier in the file).
    read_index = source.index(
        "seismic_recorder_read_session_geometry(object_id, session_id, &geometry)"
    )

    # The handler must not return early / send an error response based on
    # the geometry read's own result -- only on the (already-checked)
    # run-listing result above it.
    handler_start = source.index("static esp_err_t web_localization_runs_handler(")
    assert handler_start < read_index

    after_geometry_read = source[read_index:read_index + 400]
    assert "return httpd_resp_send_err" not in after_geometry_read


def test_geometry_appears_before_runs_in_response_order():
    source = handler_source()
    geometry_field_index = source.index('\\"geometry\\"')
    runs_field_index = source.index('\\"runs\\"')
    assert geometry_field_index < runs_field_index


def test_geometry_json_reports_every_pod_even_when_unavailable():
    source = handler_source()
    # The not-available branch must still emit one entry per pod (pod
    # index stays consistent with every other pod-indexed JSON this
    # project emits) rather than an empty array.
    for_loop_index = source.index("for (size_t pod = 0U; pod < OBJECT_STORE_POD_COUNT; ++pod)")
    loop_body = source[for_loop_index:for_loop_index + 1600]
    assert '\\"computed\\":false,\\"leveled\\":false' in loop_body
