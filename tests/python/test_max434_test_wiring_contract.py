import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TEST_SOURCES = (
    Path("components/nrf_topology_service/tests/nrf_topology_service_integration_test.c"),
    Path("tests/host/sensor_bus_concurrency_test.c"),
)
MAX440_TEST_SOURCE = Path("tests/host/ina3221_monitor_lifecycle_test.c")
MAX440_TEST_NAMES = (
    "I1_single_mutex_busy_does_not_invalidate_or_close_driver",
    "I2_owner_hold_budget_error_treated_like_busy",
    "I3_three_consecutive_device_errors_invalidate_after_backoff",
    "I4_stale_limit_10s",
    "I5_busy_then_ok_clears_stale_and_keeps_power_state",
    "I6_device_NACK_error_policy_pinned",
    "I7_period_retry_and_fast_period_tolerances",
    "I8_busy_skip_log_has_counter_age_and_rate_limit",
)
UI_PERF_TEST_SOURCE = Path("components/ui/tests/ui_perf_policy_test.c")
UI_LVGL_SOURCE = Path("components/ui/ui_lvgl.c")
UI_SOURCE = Path("components/ui/ui.c")
UI_PERF_TEST_NAMES = (
    "U1_measurement_load_policy_throttles_live_and_recording",
    "U2_flush_chunks_stay_under_5ms_and_yield_between",
    "U3_render_warning_boundary_is_strictly_over_50ms",
    "U4_old_frame_starves_150ms_budget_and_recording_cap_does_not",
    "U5_chart_refresh_interval_boundary",
    "U6_preview_caps_and_chunk_size_boundaries",
)


def _body_end(source: str, open_brace: int) -> int:
    depth = 0
    state = "code"
    index = open_brace
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 1
        elif state in ("string", "char"):
            if char == "\\":
                index += 1
            elif (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        elif char == "/" and next_char == "/":
            state = "line_comment"
            index += 1
        elif char == "/" and next_char == "*":
            state = "block_comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    raise AssertionError("unclosed C function body")


def _function_bodies(source: str, name_pattern: str) -> dict[str, list[str]]:
    functions: dict[str, list[str]] = {}
    pattern = re.compile(rf"\bstatic\s+void\s+({name_pattern})\s*\([^;]*?\)\s*\{{")
    for match in pattern.finditer(source):
        open_brace = match.end() - 1
        close_brace = _body_end(source, open_brace)
        functions.setdefault(match.group(1), []).append(source[open_brace + 1 : close_brace])
    return functions


def _called_test_names(body: str) -> set[str]:
    return set(re.findall(r"\b(test_[A-Za-z0-9_]+)\s*\(", body))


def _reachable_test_functions(source: str) -> set[str]:
    tests = _function_bodies(source, r"test_[A-Za-z0-9_]+")
    main = re.search(r"\bint\s+main\s*\([^)]*\)\s*\{", source)
    assert main, "test source has no main()"
    main_end = _body_end(source, main.end() - 1)
    reachable: set[str] = set()
    pending = _called_test_names(source[main.end() : main_end])
    while pending:
        name = pending.pop()
        if name in reachable or name not in tests:
            continue
        reachable.add(name)
        for body in tests[name]:
            pending.update(_called_test_names(body))
    return reachable


def test_max434_test_functions_are_reachable_from_main():
    for relative_path in TEST_SOURCES:
        source = (ROOT / relative_path).read_text(encoding="utf-8")
        declared = set(_function_bodies(source, r"test_[A-Za-z0-9_]+"))
        reachable = _reachable_test_functions(source)
        assert declared <= reachable, f"dead test functions in {relative_path}: {sorted(declared - reachable)}"


def test_max434_test_sources_are_registered_in_host_manifests():
    registrations = (
        (
            Path("tests/host/manifest.json"),
            "nrf_topology_service_integration",
            TEST_SOURCES[0],
        ),
        (
            Path("tests/host/manifest.d/coverage-tca.json"),
            "sensor_bus_concurrency",
            TEST_SOURCES[1],
        ),
    )
    for manifest_path, target, source_path in registrations:
        manifest = json.loads((ROOT / manifest_path).read_text(encoding="utf-8"))
        entries = [entry for entry in manifest["tests"] if entry.get("name") == target]
        assert len(entries) == 1, f"expected one {target} target in {manifest_path}"
        sources = {str(source).replace("\\", "/") for source in entries[0].get("sources", [])}
        assert source_path.as_posix() in sources, f"{source_path} is not registered in {manifest_path}"


def test_max440_cases_are_reachable_from_main():
    source = (ROOT / MAX440_TEST_SOURCE).read_text(encoding="utf-8")
    declared = set(_function_bodies(source, r"test_[A-Za-z0-9_]+"))
    reachable = _reachable_test_functions(source)
    for name in MAX440_TEST_NAMES:
        function = f"test_{name}"
        assert function in declared, f"missing MAX-440 test {function}"
        assert function in reachable, f"MAX-440 test {function} is not reachable from main()"


def test_max440_test_source_is_registered_in_host_manifests():
    for manifest_path in (
        Path("tests/host/manifest.json"),
        Path("tests/host/manifest.d/coverage-power.json"),
    ):
        manifest = json.loads((ROOT / manifest_path).read_text(encoding="utf-8"))
        entries = [
            entry for entry in manifest["tests"]
            if entry.get("name") == "ina3221_monitor_lifecycle"
        ]
        assert len(entries) == 1, f"expected one ina3221_monitor_lifecycle target in {manifest_path}"
        sources = {str(source).replace("\\", "/") for source in entries[0].get("sources", [])}
        assert MAX440_TEST_SOURCE.as_posix() in sources, (
            f"{MAX440_TEST_SOURCE} is not registered in {manifest_path}"
        )


def test_ui_perf_cases_are_reachable_from_main():
    source = (ROOT / UI_PERF_TEST_SOURCE).read_text(encoding="utf-8")
    declared = set(_function_bodies(source, r"test_[A-Za-z0-9_]+"))
    reachable = _reachable_test_functions(source)
    for name in UI_PERF_TEST_NAMES:
        function = f"test_{name}"
        assert function in declared, f"missing UI performance test {function}"
        assert function in reachable, f"UI performance test {function} is not reachable from main()"


def test_ui_perf_test_source_is_registered_in_host_manifests():
    for manifest_path in (
        Path("tests/host/manifest.json"),
        Path("tests/host/manifest.d/coverage-core.json"),
    ):
        manifest = json.loads((ROOT / manifest_path).read_text(encoding="utf-8"))
        entries = [entry for entry in manifest["tests"] if entry.get("name") == "ui_perf_policy"]
        assert len(entries) == 1, f"expected one ui_perf_policy target in {manifest_path}"
        sources = {str(source).replace("\\", "/") for source in entries[0].get("sources", [])}
        assert UI_PERF_TEST_SOURCE.as_posix() in sources, (
            f"{UI_PERF_TEST_SOURCE} is not registered in {manifest_path}"
        )


def test_flush_wait_uses_completion_barrier_with_tick_fallback():
    source = (ROOT / UI_LVGL_SOURCE).read_text(encoding="utf-8")
    assert "vTaskDelay(1U);" in source
    assert "pdMS_TO_TICKS(1U)" not in source
    assert "if (ui_lvgl_drain_panel_io_locked() != ESP_OK)" in source
    assert "s_flush_wait_yielded = s_measurement_load_active;" not in source


def test_measurement_load_policy_uses_screen_and_acquisition_state():
    source = (ROOT / UI_SOURCE).read_text(encoding="utf-8")
    callbacks = _function_bodies(source, "ui_refresh_timer_callback")
    assert len(callbacks.get("ui_refresh_timer_callback", [])) == 1
    body = callbacks["ui_refresh_timer_callback"][0]
    assert "s_context.screen == UI_SCREEN_MEASUREMENTS" in body
    assert "acquisition.running" in body
    assert "ui_perf_policy_measurements_load_active" in body
    assert "measurement.state == MEASUREMENT_SERVICE_RECORDING" not in body


def test_key_events_are_dispatched_before_measurement_load_throttle_return():
    source = (ROOT / UI_SOURCE).read_text(encoding="utf-8")
    callbacks = _function_bodies(source, "ui_refresh_timer_callback")
    body = callbacks["ui_refresh_timer_callback"][0]
    dispatch = body.index("ui_event_dispatcher_dispatch_pending_locked();")
    throttle_return = body.index("if (now_us < s_measurement_load_next_update_us) return;")
    assert dispatch < throttle_return


def test_measurement_chart_limits_follow_running_acquisition_snapshot():
    source = (ROOT / Path("components/ui/ui_measurements.c")).read_text(encoding="utf-8")
    assert "acquisition != NULL && acquisition->running" in source
    assert "ui_perf_policy_preview_frame_limit(requested_frames, load_limited)" in source
    assert "ui_perf_policy_data_prep_budget_exhausted(elapsed_us)" in source
