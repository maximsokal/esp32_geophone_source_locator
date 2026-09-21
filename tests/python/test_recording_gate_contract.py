"""Source contracts for the recording gate policy (MAX-165)."""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (REPO_ROOT / relative_path).read_text(encoding="utf-8")


def test_reuses_dimensionality_policy_instead_of_duplicating_the_threshold():
    """The pre-recording gate and the post-recording localization
    method-availability decision (MAX-170) must always agree on the 3-vs-4
    threshold, so this must call the shared classifier rather than
    hardcoding its own copy of '3' or '4'."""
    source = read("components/recording_gate/recording_gate_policy.c")
    assert "localization_dimensionality_classify(" in source


def test_has_no_esp_idf_or_topology_service_dependency():
    """Pure and stateless: takes plain counts, not a topology struct, so it
    stays host-testable without any ESP-IDF stub."""
    cmake = read("components/recording_gate/CMakeLists.txt")
    header = read("components/recording_gate/include/recording_gate.h")

    assert "nrf_topology" not in cmake
    assert '#include "nrf_topology' not in header
    assert '#include "esp_err' not in header


def test_blocked_decisions_have_reasons_allowed_decisions_do_not_invent_one():
    source = read("components/recording_gate/recording_gate_policy.c")

    assert "RECORDING_GATE_BLOCKED_INSUFFICIENT_REMOTES" in source
    assert "RECORDING_GATE_BLOCKED_NOT_LEVEL" in source
    assert 'return "";' in source
