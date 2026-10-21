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


def test_max449_navigation_and_low_remote_tests_are_registered():
    targets = _tests(ROOT / "tests/host/manifest.d/max449-measurements-workflow.json")
    expected = {
        "max449_n1_home_measurements_redirect",
        "max449_n2_home_measurements_object_picker",
        "max449_n3_measurements_state_matrix",
        "max449_n4_low_remote_workflow",
        "max449_n7_objects_start_measurement",
        "max449_n5_workflow_gate_strict",
        "max449_n6_ui_sensor_check_default",
        "max449_n6_ui_sensor_check_strict",
        "max449_n4_insufficient_node_result_text",
        "max449_n7_workflow_preview_regression",
    }
    assert expected <= targets.keys()

    navigation = (ROOT / "tests/host/ui_home_navigation_test.c").read_text(encoding="utf-8")
    for case, function in (
        (1, "test_home_measurements_redirects_active_object_for_ok_and_right"),
        (2, "test_home_measurements_without_active_object_opens_workflow_picker"),
        (3, "test_measurements_route_decision_all_workflow_states"),
    ):
        assert function in navigation
        assert f"#if MAX449_CASE == {case}" in navigation or f"#elif MAX449_CASE == {case}" in navigation
        assert "tests/host/ui_home_navigation_test.c" in targets[
            next(name for name in expected if name.startswith(f"max449_n{case}_"))
        ]["sources"]

    service = (ROOT / "tests/host/workflow_service_test.c").read_text(encoding="utf-8")
    assert "test_low_remote_calibration_timeout_and_pair_counts();" in service
    assert "test_low_remote_placement_and_ranging_finish();" in service
    assert "MAX449_CASE=4" in targets["max449_n4_low_remote_workflow"]["defines"]
    assert "CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING=0" in targets[
        "max449_n4_low_remote_workflow"
    ]["defines"]

    core = (ROOT / "tests/host/workflow_core_test.c").read_text(encoding="utf-8")
    assert "test_go_calibration_initializes_any_connected_count();" in core
    strict_init = _tests(ROOT / "tests/host/manifest.d/workflow-low-count-initialization.json")[
        "workflow_low_count_initialization_strict"
    ]
    assert "MAX449_CASE=4" in strict_init["defines"]
    assert "CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING=1" in strict_init["defines"]
    assert "components/workflow_service/workflow_service.c" in strict_init["sources"]
    assert "test_one_and_two_connected_remotes_complete_initialization();" in service
    assert "CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING=1" in targets[
        "max449_n5_workflow_gate_strict"
    ]["defines"]

    model = (ROOT / "components/ui/tests/ui_workflow_model_test.c").read_text(encoding="utf-8")
    assert "test_sensor_check_action_and_warning_for_low_remote_counts();" in model
    assert "test_workflow_start_and_preview_regressions();" in model
    assert "test_insufficient_nodes_results_use_existing_warning();" in model
    assert "MAX449_CASE=6" in targets["max449_n6_ui_sensor_check_default"]["defines"]
    assert "MAX449_CASE=6" in targets["max449_n6_ui_sensor_check_strict"]["defines"]
    assert "MAX449_CASE=8" in targets["max449_n4_insufficient_node_result_text"]["defines"]
    assert "MAX449_CASE=4" in targets["max449_n7_objects_start_measurement"]["defines"]
    assert "tests/host/ui_home_navigation_test.c" in targets[
        "max449_n7_objects_start_measurement"
    ]["sources"]
    assert "MAX449_CASE=7" in targets["max449_n7_workflow_preview_regression"]["defines"]

    objects_input = (ROOT / "components/ui/ui_objects_input.c").read_text(encoding="utf-8")
    objects = (ROOT / "components/ui/ui_objects.c").read_text(encoding="utf-8")
    preview = (ROOT / "components/ui/ui_scan_preview.c").read_text(encoding="utf-8")
    assert "UI_OBJECTS_SELECT_WORKFLOW" in objects_input
    assert "ui_screen_open_locked(UI_SCREEN_WORKFLOW);" in objects_input
    assert "OBJECT_SERVICE_EVENT_SET_ACTIVE_DONE" in objects
    assert "context->workflow_activation_pending" in objects
    assert "if (s_from_workflow) ui_screen_open_locked(UI_SCREEN_WORKFLOW);" in preview
