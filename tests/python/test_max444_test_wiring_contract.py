import json
import os
from pathlib import Path


ROOT = Path(os.environ.get("MAX444_TEST_ROOT", Path(__file__).resolve().parents[2])).resolve()
TARGET_NAMES = {
    "recording_gate_bypass_policy",
    "recording_gate_required_policy",
    "seismic_recorder_gate_bypass_start_0",
    "seismic_recorder_gate_bypass_start_1",
    "seismic_recorder_gate_bypass_start_2",
    "web_status_recording_gate",
    "seismic_recorder_session_geometry",
    "seismic_recorder_start_integration",
    "nrf_geometry_solver",
}


def _tests(path: Path) -> dict[str, dict]:
    return {entry["name"]: entry for entry in json.loads(path.read_text(encoding="utf-8"))["tests"]}


def test_recording_gate_and_start_tests_are_registered_and_called():
    manifest = _tests(ROOT / "tests/host/manifest.json")
    fragment = _tests(ROOT / "tests/host/manifest.d/max444-recording-gate.json")
    assert TARGET_NAMES <= manifest.keys()
    assert TARGET_NAMES <= fragment.keys()

    for target_name in ("recording_gate_bypass_policy", "recording_gate_required_policy"):
        entry = manifest[target_name]
        assert "components/recording_gate/tests/recording_gate_bypass_test.c" in entry["sources"]
    policy_test = (ROOT / "components/recording_gate/tests/recording_gate_bypass_test.c").read_text(
        encoding="utf-8"
    )
    assert "int main(void)" in policy_test
    assert "recording_gate_evaluate(" in policy_test

    start_source = "tests/host/seismic_recorder_start_integration_test.c"
    for remote_count in range(3):
        entry = manifest[f"seismic_recorder_gate_bypass_start_{remote_count}"]
        assert start_source in entry["sources"]
        assert f"SEISMIC_RECORDER_GATE_BYPASS_REMOTE_COUNT={remote_count}" in entry["defines"]
        assert "SEISMIC_RECORDER_GATE_BYPASS_TEST=1" in entry["defines"]

    strict_defines = manifest["seismic_recorder_start_integration"]["defines"]
    assert "CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING=1" in strict_defines


def test_metadata_and_status_regressions_are_in_main():
    session_test = (
        ROOT / "components/seismic_recorder/tests/seismic_recorder_session_geometry_test.c"
    ).read_text(encoding="utf-8")
    assert "static void test_recording_gate_metadata_round_trips(void)" in session_test
    assert "test_recording_gate_metadata_round_trips();" in session_test

    web_test = (ROOT / "tests/host/web_status_recording_gate_host_test.c").read_text(
        encoding="utf-8"
    )
    assert "test_insufficient_gate_is_serialized_as_blocked();" in web_test
    assert 'assert_bool(gate, "bypassed", true);' in web_test

    solver_test = (ROOT / "tests/host/nrf_geometry_solver_test.c").read_text(
        encoding="utf-8"
    )
    assert "test_less_than_three_remote_nodes_return_unsolved();" in solver_test
    assert "test_less_than_three_remote_nodes_return_unsolved(void)" in solver_test
    solver_defines = _tests(ROOT / "tests/host/manifest.json")["nrf_geometry_solver"]["defines"]
    assert "NRF_GEOMETRY_SOLVER_HOST_TEST=1" in solver_defines


def test_default_bypass_is_pinned_in_kconfig_and_project_defaults():
    kconfig_path = ROOT / "components/recording_gate/Kconfig"
    assert kconfig_path.is_file(), "recording gate Kconfig must pin the default"
    kconfig = kconfig_path.read_text(encoding="utf-8")
    defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
    assert "config RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING" in kconfig
    assert "\tdefault n" in kconfig
    assert "# CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING is not set" in defaults
