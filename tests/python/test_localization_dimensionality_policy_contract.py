"""Source contracts for the localization dimensionality policy (MAX-170)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_no_measured_constant_lives_in_the_policy():
    """Only the ticket's own 3/4-remote thresholds may appear here -- no
    filter band, propagation speed, or other measured/guessed value."""
    header = read("components/localization/include/localization_dimensionality_policy.h")
    source = read("components/localization/localization_dimensionality_policy.c")

    assert "propagation_speed" not in header
    assert "propagation_speed" not in source
    assert "default_config" not in header
    assert "default_config" not in source


def test_bridge_distinguishes_insufficient_from_method_unavailable():
    """MAX-170: exactly 3 accepted channels is a valid planar/2D session,
    not a generic 'insufficient data' failure -- the wired 3D-only TDOA
    solver just doesn't support it. These must be reported as two distinct
    statuses so a future 2D-capable method could still run."""
    header = read("components/localization/include/localization_event_bridge.h")
    bridge = read("components/localization/localization_event_bridge.c")

    assert "LOCALIZATION_EVENT_INSUFFICIENT_CHANNELS" in header
    assert "LOCALIZATION_EVENT_METHOD_UNAVAILABLE_FOR_DIMENSIONALITY" in header
    assert "localization_dimensionality_classify(" in bridge
    assert "localization_method_availability(" in bridge
    assert "LOCALIZATION_METHOD_REQUIRES_3D" in bridge


def test_bridge_result_always_reports_dimensionality():
    header = read("components/localization/include/localization_event_bridge.h")
    assert "localization_dimensionality_t dimensionality;" in header


def test_solver_files_still_unaware_of_dimensionality_policy():
    """localization.c/.h stay pure TDOA math; only the bridge and the policy
    file itself know about dimensionality classification."""
    solver = read("components/localization/localization.c")
    solver_header = read("components/localization/include/localization.h")

    assert "dimensionality" not in solver.lower()
    assert "dimensionality" not in solver_header.lower()
