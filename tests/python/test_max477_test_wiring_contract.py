"""Keep the MAX-477 health policy's real host tests on both manifest surfaces."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEALTH_SOURCE = "components/runtime_observability/runtime_health_policy.c"
HEALTH_TEST = "tests/host/runtime_health_policy_test.c"
OBSERVABILITY_TEST = "tests/host/runtime_observability_contract_test.c"
RECORDER_TEST = "tests/host/seismic_recorder_start_integration_test.c"


def _entry(manifest: dict, name: str) -> dict:
    entries = [entry for entry in manifest["tests"] if entry["name"] == name]
    assert len(entries) == 1, f"expected one {name} target"
    return entries[0]


def _assert_health_policy_registered():
    manifest = json.loads((ROOT / "tests/host/manifest.json").read_text(encoding="utf-8"))
    _entry(manifest, "runtime_health_policy")


def test_health_policy_targets_are_registered_in_both_host_manifests():
    manifests = (
        ROOT / "tests/host/manifest.json",
        ROOT / "tests/host/manifest.d/max477-health-policy.json",
    )
    for path in manifests:
        manifest = json.loads(path.read_text(encoding="utf-8"))
        health = _entry(manifest, "runtime_health_policy")
        assert HEALTH_SOURCE in health["sources"]
        assert HEALTH_TEST in health["sources"]
        observability = _entry(manifest, "runtime_observability")
        assert HEALTH_SOURCE in observability["sources"]
        assert OBSERVABILITY_TEST in observability["sources"]
        assert HEALTH_SOURCE in _entry(manifest, "runtime_observability_policy")["sources"]


def test_behavioral_cases_are_called_from_the_host_test_main():
    _assert_health_policy_registered()
    source = (ROOT / HEALTH_TEST).read_text(encoding="utf-8")
    main = source.split("int main(void)", 1)[1]
    for name in (
        "test_live_heartbeat_does_not_hide_frozen_progress",
        "test_allowed_wait_is_quiet_until_its_deadline",
        "test_progress_deadline_boundaries",
        "test_reaction_order_and_hard_failure_gate",
        "test_missing_expected_task_blocks_completeness",
    ):
        assert f"{name}();" in main
    observability = (ROOT / OBSERVABILITY_TEST).read_text(encoding="utf-8")
    assert "runtime_observability_get_health_result(RUNTIME_TASK_STORAGE)" in observability
    assert "runtime_observability_get_health_result(RUNTIME_TASK_CONTROL)" in observability


def test_timeout_incomplete_invariant_is_exercised_by_recorder_host_target():
    _assert_health_policy_registered()
    source = (ROOT / RECORDER_TEST).read_text(encoding="utf-8")
    main = source.split("int main(void)", 1)[1]
    assert 'seismic_recorder_start("OBJ_0007")' in main
    assert "complete_metadata_writes_after_deadline == 0U" in main
    manifest = json.loads((ROOT / "tests/host/manifest.json").read_text(encoding="utf-8"))
    assert RECORDER_TEST in _entry(manifest, "seismic_recorder_start_integration")["sources"]


def test_runtime_component_builds_the_health_policy():
    cmake = (ROOT / "components/runtime_observability/CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    assert '"runtime_health_policy.c"' in cmake
