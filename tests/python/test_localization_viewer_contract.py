"""Source contracts for MAX-148: exposing this session's own frozen sensor
geometry (X/Y/Z, leveled), CS pair evidence (distance/confidence/valid) and
per-channel arrival-pick evidence, alongside its persisted localization
runs -- all best-effort, never a hard failure of the listing route -- plus
the persistence paths that freeze CS pairs into object_store/session.json
alongside the existing geometry freeze, and pick evidence into its own
computed_at_ms-versioned record per "Calculate source" invocation."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def handler_source() -> str:
    return read("components/web_server/web_localization_api.c")


def object_store_header() -> str:
    return read("components/object_store/include/object_store.h")


def prepare_geometry_source() -> str:
    return read("components/seismic_recorder/seismic_recorder.c")


def orchestrator_source() -> str:
    return read("components/localization_calculate/localization_calculate.c")


def run_store_header() -> str:
    return read("components/localization_run_store/include/localization_run_store.h")


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


def test_cs_pairs_read_never_uses_object_live_topology():
    source = handler_source()
    assert "seismic_recorder_read_session_cs_pairs(object_id, session_id, &cs_pairs)" in source


def test_cs_pairs_read_failure_does_not_fail_the_request():
    source = handler_source()
    read_index = source.index(
        "seismic_recorder_read_session_cs_pairs(object_id, session_id, &cs_pairs)"
    )
    after_read = source[read_index:read_index + 400]
    assert "return httpd_resp_send_err" not in after_read


def test_response_field_order_is_geometry_then_cs_pairs_then_picks_then_runs():
    source = handler_source()
    geometry_index = source.index('\\"geometry\\"')
    cs_pairs_index = source.index('\\"cs_pairs\\"')
    picks_index = source.index('\\"picks\\"')
    runs_index = source.index('\\"runs\\"')
    assert geometry_index < cs_pairs_index < picks_index < runs_index


def test_picks_listing_failure_does_not_fail_the_request():
    source = handler_source()
    list_index = source.index(
        "localization_run_store_list_picks_ex(\n"
        "            object_id, session_id, picks, LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION,\n"
        "            &pick_count, &picks_list_status\n"
        "        )"
    )
    after_list = source[list_index:list_index + 400]
    assert "return httpd_resp_send_err" not in after_list


def test_pick_record_schema_exists_and_is_decoupled_from_object_store():
    header = read("components/localization_results/include/localization_pick_record.h")
    # Reuses geophone_channel_pick_t verbatim (the established shared shape
    # for "one channel's pick result" -- localization_event_bridge.h
    # already embeds it the same way) rather than a local channel count
    # dependency on object_store.h.
    assert "geophone_channel_pick_t pick;" in header
    assert '#include "object_store.h"' not in header
    assert "LOCALIZATION_PICK_RECORD_CHANNEL_COUNT" in header


def test_run_store_provides_a_pick_record_write_and_list_analog():
    header = run_store_header()
    assert "localization_run_store_write_picks(" in header
    assert "localization_run_store_list_picks(" in header
    assert "localization_run_store_build_picks_path(" in header


def test_orchestrator_persists_pick_evidence_before_any_method_record():
    source = orchestrator_source()
    pick_write_index = source.index(
        "pick_write_err = localization_calculate_write_picks_retry("
    )
    first_method_write_index = source.index("localization_run_store_write(object_id, session_id, &records[i]);")
    assert pick_write_index < first_method_write_index


def test_object_store_defines_a_decoupled_cs_pair_schema():
    header = object_store_header()
    # Its own persistence-facing struct, not nrf_cs_pair_result_t reused
    # verbatim -- matching the same decoupling object_store_pod_geometry_t
    # already establishes for solved geometry.
    assert "object_store_cs_pair_t" in header
    assert "object_store_cs_pairs_t" in header
    assert "object_store_cs_pairs_t cs_pairs;" in header
    assert "object_store_set_cs_pairs(" in header


def test_cs_pairs_are_persisted_from_the_same_completed_cs_cycle_as_geometry():
    source = prepare_geometry_source()

    geometry_write_index = source.index("object_store_set_geometry(object_id, &geometry);")
    cs_pairs_write_index = source.index("object_store_set_cs_pairs(object_id, session_cs_pairs);")
    # CS pairs are frozen right after geometry, from the same solve --
    # never before it (which would risk freezing a stale/prior cycle).
    assert geometry_write_index < cs_pairs_write_index

    # Unlike the geometry write, a CS-pairs persistence failure must not
    # reject START -- it is a best-effort placement-confidence extra, not
    # the recording-gate's own admission promise.
    window = source[cs_pairs_write_index:cs_pairs_write_index + 300]
    assert "return err" not in window
    assert "return cs_pairs_err" not in window
