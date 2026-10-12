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


def test_gate_is_actually_wired_into_the_real_recording_admission_path():
    """A tested-but-unwired pure function does not protect production: this
    pins that seismic_recorder's START path applies the shared gate result.
    The default bypass can admit a start while preserving the diagnostic
    decision; strict configuration still blocks it."""
    recorder = read("components/seismic_recorder/seismic_recorder.c")
    cmake = read("components/seismic_recorder/CMakeLists.txt")

    assert '#include "recording_gate.h"' in recorder
    assert "recording_gate_evaluate(" in recorder
    assert "RECORDING_GATE_BLOCKED_INSUFFICIENT_REMOTES" in recorder
    assert "if (!gate.start_allowed)" in recorder
    assert "recording_gate_decision_reason(gate.decision)" in recorder
    assert "recording_gate_bypass_reason(recording_gate.decision)" in recorder
    assert "RECORDING_GATE_BLOCKED_NOT_LEVEL" not in recorder
    assert "RECORDING_GATE_ALLOWED_PLANAR_2D" in recorder
    assert "recording_gate" in cmake

    # The old MAX-118-era leveling check this supersedes must be gone, not
    # left as unreachable dead code alongside the real gate.
    assert "seismic_recorder_geometry_validate_leveling" not in recorder
    policy_header = read("components/seismic_recorder/seismic_recorder_geometry_policy.h")
    policy_source = read("components/seismic_recorder/seismic_recorder_geometry_policy.c")
    assert "seismic_recorder_geometry_validate_leveling" not in policy_header
    assert "seismic_recorder_geometry_validate_leveling" not in policy_source


def test_gate_is_exposed_in_extended_status_and_consumable_by_the_webui():
    """The UI gate must be present in the same status response that drives
    the Record button, not only in an isolated JSON helper."""
    status_builder = read("components/web_server/web_status_json.c")
    recording_json = read("components/web_server/web_status_json_recording.c")
    internal_header = read("components/web_server/web_server_internal.h")
    web_cmake = read("components/web_server/CMakeLists.txt")
    webui_device = read("webui/src/domain/device.ts")
    webui_feature = read("webui/src/features/objects/ObjectsFeature.tsx")

    assert "web_status_json_add_recording_gate(root)" in status_builder
    assert "bool web_status_json_add_recording_gate(cJSON *root);" in internal_header
    assert '"recording_gate"' in recording_json
    assert "nrf_topology_service" in web_cmake
    assert "recording_gate" in web_cmake
    assert "RecordingGateStatus" in webui_device
    assert "status?.recording_gate" in webui_feature
    assert "recordingGateBlocksStart" in webui_feature
