"""Source contracts for the localization <-> geophone_events bridge (MAX-135)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_solver_file_is_unchanged_by_the_bridge():
    """MAX-135: 'wire ... without merging arrival picking into the solver.'
    localization.c must stay pure TDOA math with no geophone_events
    dependency; only the new bridge file may know about picks."""
    solver = read("components/localization/localization.c")
    solver_header = read("components/localization/include/localization.h")

    assert "geophone_events.h" not in solver
    assert "geophone_events.h" not in solver_header
    assert "geophone_event_" not in solver
    assert "geophone_event_" not in solver_header


def test_bridge_never_invents_a_propagation_speed():
    bridge = read("components/localization/localization_event_bridge.c")
    bridge_header = read("components/localization/include/localization_event_bridge.h")

    assert "propagation_speed_mps =" not in bridge
    assert "propagation_speed_mps =" not in bridge_header
    assert "default_config" not in bridge


def test_bridge_reports_channel_mask_and_explicit_rejection_status():
    header = read("components/localization/include/localization_event_bridge.h")

    assert "channel_mask_used" in header
    assert "channel_mask_rejected" in header
    assert "LOCALIZATION_EVENT_INSUFFICIENT_CHANNELS" in header
    assert "LOCALIZATION_EVENT_SOLVER_REJECTED" in header
    assert "LOCALIZATION_EVENT_SOLVED" in header


def test_bridge_converts_time_from_exact_sample_index_not_float_seconds():
    """arrival_time_s is float32; at real recording lengths it can no longer
    resolve microseconds, so the bridge must use arrival_sample_index +
    sample_rate_hz (integer/double) for the conversion instead."""
    bridge = read("components/localization/localization_event_bridge.c")

    assert "arrival_sample_index" in bridge
    assert ".arrival_time_us =" in bridge
    conversion = bridge[bridge.index("const double arrival_time_us") :]
    assert "pick.arrival_time_s" not in conversion.split("\n\n")[0]


def test_localization_component_declares_geophone_events_dependency():
    cmake = read("components/localization/CMakeLists.txt")
    assert "geophone_events" in cmake
    assert "localization_event_bridge.c" in cmake
