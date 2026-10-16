import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tests/host/manifest.d/max481b-queue-semantics.json"


def test_max481b_queue_lifetime_and_copy_contracts_are_registered():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    entries = {entry["name"]: entry for entry in manifest["tests"]}
    assert "components/measurement_service/tests/measurement_service_worker_contract_test.c" in entries[
        "max481b_measurement_queue_contract"
    ]["sources"]
    assert "tests/host/storage_service_lifecycle_test.c" in entries[
        "max481b_storage_queue_contract"
    ]["sources"]

    cases = {
        ROOT / "components/measurement_service/tests/measurement_service_worker_contract_test.c": (
            "test_submit_finishes_before_queue_teardown",
            "test_queue_creation_failure_has_no_partial_service",
        ),
        ROOT / "tests/host/storage_service_lifecycle_test.c": (
            "test_submit_finishes_before_storage_queue_teardown",
            "test_submit_failure_returns_timeout_and_releases_gate",
            "test_queue_creation_failure_returns_no_memory",
            "test_shutdown_send_failure_retains_queue",
            "test_receive_timeout_ignores_stale_output",
        ),
    }
    for path, names in cases.items():
        source = path.read_text(encoding="utf-8")
        for name in names:
            assert f"static void {name}(void)" in source
            assert f"    {name}();" in source
