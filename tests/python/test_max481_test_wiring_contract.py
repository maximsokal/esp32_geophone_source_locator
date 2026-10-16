import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFESTS = (
    ROOT / "tests/host/manifest.json",
    *sorted((ROOT / "tests/host/manifest.d").glob("*.json")),
)
SMALL_TICK_ALLOWLIST = {
    # This component is config-disabled; its waits are outside MAX-483 scope.
    "components/insights_bridge/insights_bridge.c": 2,
}


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


def _functions(source: str) -> dict[str, str]:
    functions: dict[str, str] = {}
    pattern = re.compile(r"\bstatic\s+void\s+(test_[A-Za-z0-9_]+)\s*\([^;]*?\)\s*\{")
    for match in pattern.finditer(source):
        end = _body_end(source, match.end() - 1)
        functions[match.group(1)] = source[match.end() : end]
    return functions


def _reachable(source: str) -> set[str]:
    functions = _functions(source)
    main = re.search(r"\bint\s+main\s*\([^)]*\)\s*\{", source)
    assert main, "host test has no main()"
    end = _body_end(source, main.end() - 1)
    pending = set(re.findall(r"\b(test_[A-Za-z0-9_]+)\s*\(", source[main.end() : end]))
    reached: set[str] = set()
    while pending:
        name = pending.pop()
        if name in reached or name not in functions:
            continue
        reached.add(name)
        pending.update(re.findall(r"\b(test_[A-Za-z0-9_]+)\s*\(", functions[name]))
    return reached


def _manifest_entry(name: str, source_path: str) -> dict:
    entries = []
    for manifest in MANIFESTS:
        document = json.loads(manifest.read_text(encoding="utf-8"))
        entries.extend(
            entry
            for entry in document["tests"]
            if entry.get("name") == name
            and source_path in {str(path).replace("\\", "/") for path in entry.get("sources", [])}
        )
    assert entries, f"expected {source_path} in registered target {name}"
    return entries[0]


def test_max481_cases_are_reachable_from_main_and_registered():
    cases = (
        (
            "max481_app_storage_boot_ui",
            "tests/host/app_storage_boot_ui_test.c",
            ("full_event_queue_counts_loss_and_prioritizes_shutdown",),
        ),
        (
            "max481_seismic_buffer",
            "components/seismic_buffer/tests/seismic_buffer_test.c",
            (
                "queue_stats_track_n_boundaries_and_reader_drops",
                "overlapping_producers_are_rejected",
                "slot_sequence_rollover_fails_closed",
                "64_bit_frame_and_cursor_exhaustion_fails_closed",
            ),
        ),
        (
            "max481_seismic_recorder_cache",
            "components/seismic_recorder/tests/seismic_recorder_cache_test.c",
            ("source_and_ring_gap_accounting", "source_gaps_survive_cache_slot_wrap"),
        ),
        (
            "max481_seismic_recorder_fs_v2_writer",
            "components/seismic_recorder/tests/seismic_recorder_fs_contract_test.c",
            (
                "acquisition_gap_survives_ring_cache_v2_reader",
                "ring_overflow_survives_as_recorded_gap",
            ),
        ),
        (
            "max481_measurement_service",
            "components/measurement_service/tests/measurement_service_worker_contract_test.c",
            ("full_queue_stop_then_recorder_fault_never_reports_stopped_ok",),
        ),
        (
            "max481_workflow_service",
            "tests/host/workflow_service_test.c",
            ("async_fifo_stale_generation_full_queue_and_monotonic_sequence",),
        ),
        (
            "max481_seismic_recorder_start_integration",
            "tests/host/seismic_recorder_start_integration_test.c",
            ("source_gap_summary_is_written",),
        ),
        (
            "max481_seismic_recorder_session_geometry",
            "components/seismic_recorder/tests/seismic_recorder_session_geometry_test.c",
            ("summary_gap_classes_round_trip",),
        ),
        (
            "main_barometer",
            "tests/host/main_barometer_test.c",
            ("aht20_monotonic_deadline_boundaries_and_minimum_tick",),
        ),
        (
            "app_tick_conversion",
            "tests/host/app_tick_test.c",
            ("subtick_conversion_is_never_zero_for_positive_waits",),
        ),
        (
            "max479_ad7606_lifecycle",
            "components/ad7606_acquisition/tests/ad7606_acquisition_runtime_test.c",
            ("stop_isr_worker_lifecycle_interleaving",),
        ),
        (
            "nrf_topology_service_ranging_integration",
            "components/nrf_topology_service/tests/nrf_topology_service_integration_test.c",
            ("height_calibration_worker_queue_and_generation_wrap",),
        ),
    )
    for target, source_path, names in cases:
        entry = _manifest_entry(target, source_path)
        sources = {str(path).replace("\\", "/") for path in entry.get("sources", [])}
        assert source_path in sources, f"{source_path} is not in target {target}"
        source = (ROOT / source_path).read_text(encoding="utf-8")
        declared = _functions(source)
        reached = _reachable(source)
        for name in names:
            function = f"test_{name}"
            assert function in declared, f"missing {function} in {source_path}"
            assert function in reached, f"{function} is not called by main() in {source_path}"


def test_project_small_literal_tick_delays_are_explicit():
    pattern = re.compile(r"pdMS_TO_TICKS\s*\(\s*(\d+)\s*(?:U|UL|u|ul)?\s*\)")
    observed: dict[str, int] = {}
    for source_root in (ROOT / "main", ROOT / "components"):
        for path in source_root.rglob("*"):
            if not path.is_file() or path.suffix not in {".c", ".h"} or "tests" in path.parts:
                continue
            relative = path.relative_to(ROOT).as_posix()
            count = sum(
                1
                for value in pattern.findall(path.read_text(encoding="utf-8"))
                if int(value) < 10
            )
            if count:
                observed[relative] = count
    assert observed == SMALL_TICK_ALLOWLIST, (
        "small literal pdMS_TO_TICKS calls need an explicit fix or reviewed allowlist: "
        f"{observed}"
    )
