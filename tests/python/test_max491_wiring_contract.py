import json
import os
from pathlib import Path


ROOT = Path(os.environ.get("MAX491_TEST_ROOT", Path(__file__).resolve().parents[2])).resolve()
MANIFEST = ROOT / "tests/host/manifest.d/max491-device-fsm.json"
MUTATIONS = ROOT / "tools/mutations/max491_device_fsm.json"


def _tests(path: Path) -> dict[str, dict]:
    return {entry["name"]: entry for entry in json.loads(path.read_text(encoding="utf-8"))["tests"]}


# MAX-491 mutation-only: the new fragment reaches every added test source.
def test_max491_manifest_registers_model_service_ui_job_and_recorder_tests():
    tests = _tests(MANIFEST)
    expected = {
        "workflow_fsm_model",
        "workflow_fsm_model_strict",
        "workflow_service_max491",
        "ui_workflow_model_max491",
        "localization_job_state_max491",
        "seismic_recorder_fault_fsm_strict",
        "seismic_recorder_fault_fsm_bypass",
    }
    assert expected == tests.keys()
    assert "tests/host/workflow_fsm_model_test.c" in tests["workflow_fsm_model"]["sources"]
    assert "tests/host/workflow_fsm_model_test.c" in tests["workflow_fsm_model_strict"]["sources"]
    assert "CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING=1" in tests[
        "workflow_fsm_model_strict"
    ]["defines"]
    assert "tests/host/workflow_service_test.c" in tests["workflow_service_max491"]["sources"]
    assert "components/ui/tests/ui_workflow_model_test.c" in tests["ui_workflow_model_max491"]["sources"]
    assert "components/localization_job/tests/localization_job_state_test.c" in tests[
        "localization_job_state_max491"
    ]["sources"]
    for name, required_gate in (
        ("seismic_recorder_fault_fsm_strict", "CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING=1"),
        ("seismic_recorder_fault_fsm_bypass", "CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING=0"),
    ):
        assert "tests/host/seismic_recorder_fault_fsm_test.c" in tests[name]["sources"]
        assert required_gate in tests[name]["defines"]
        assert "malloc=seismic_recorder_fault_malloc" in tests[name]["defines"]

    model = (ROOT / "tests/host/workflow_fsm_model_test.c").read_text(encoding="utf-8")
    assert "assert(WORKFLOW_STEP_COUNT == 19);" in model
    assert "assert(WORKFLOW_COMMAND_COUNT == 25);" in model
    assert "static void test_all_state_command_pairs(void)" in model
    assert "static void test_seeded_action_sequences(void)" in model
    assert "shrink_sequence(actions, &count, seed);" in model

    service = (ROOT / "tests/host/workflow_service_test.c").read_text(encoding="utf-8")
    assert "static void test_service_fsm_illegal_events_and_single_recording_lifecycle(void)" in service
    assert "test_service_fsm_illegal_events_and_single_recording_lifecycle();" in service

    ui = (ROOT / "components/ui/tests/ui_workflow_model_test.c").read_text(encoding="utf-8")
    assert "test_async_sequence_wrap_keeps_new_request_pending();" in ui
    assert "test_material_save_sequence_wrap_keeps_new_save_pending();" in ui

    job = (ROOT / "components/localization_job/tests/localization_job_state_test.c").read_text(encoding="utf-8")
    assert "test_job_id_wrap_skips_zero();" in job
    assert "test_duplicate_finish_is_immutable();" in job


# MAX-491 mutation-only: every mutation names a runnable target and a called killer.
def test_max491_mutations_point_to_registered_test_killers():
    tests = _tests(MANIFEST)
    mutations = json.loads(MUTATIONS.read_text(encoding="utf-8"))
    expected = {
        "max491_remove_generation_guard": (
            "workflow_fsm_model",
            "tests/host/workflow_fsm_model_test.c",
            "test_guard_boundaries_and_generation",
        ),
        "max491_allow_duplicate_job_finish": (
            "localization_job_state_max491",
            "components/localization_job/tests/localization_job_state_test.c",
            "test_duplicate_finish_is_immutable",
        ),
        "max491_allow_count_transition": (
            "workflow_fsm_model",
            "tests/host/workflow_fsm_model_test.c",
            "test_all_state_command_pairs",
        ),
        "max491_drop_stop_stage_error": (
            "seismic_recorder_fault_fsm_strict",
            "tests/host/seismic_recorder_fault_fsm_test.c",
            "test_stop_stage_faults_never_mark_partial_data_complete",
        ),
    }
    # max491_drop_stop_stage_error targets seismic_recorder.c, which has mixed line
    # endings, so tools/run_mutation_tests.py refuses it; it is verified on a scratch copy.
    assert {item["id"] for item in mutations} == expected.keys() - {"max491_drop_stop_stage_error"}
    for mutation in mutations:
        target_name, test_path, function = expected[mutation["id"]]
        assert target_name in tests
        assert any(
            test["manifest"] == "tests/host/manifest.d/max491-device-fsm.json"
            and test["only"] == target_name
            for test in mutation["tests"]
        )
        source = (ROOT / test_path).read_text(encoding="utf-8")
        assert f"{function}(void)" in source
        assert f"{function}();" in source
        assert mutation["expected_killer"].endswith(f"::{function}")

    recorder = (ROOT / "tests/host/seismic_recorder_fault_fsm_test.c").read_text(encoding="utf-8")
    for name in ("test_early_failures_preserve_public_state", "test_session_creation_and_start_metadata_failures",
                 "test_stop_stage_faults_never_mark_partial_data_complete"):
        assert f"{name}();" in recorder
    assert '#include "seismic_recorder_start_integration_test.c"' in recorder
