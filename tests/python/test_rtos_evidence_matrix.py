from __future__ import annotations

import json
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.rtos_evidence import check_rtos_evidence as checker  # noqa: E402
from tools import run_local_verify  # noqa: E402


MATRIX = ROOT / "tools/rtos_evidence/rtos_evidence_matrix.json"


def load_matrix() -> dict:
    return json.loads(MATRIX.read_text(encoding="utf-8"))


def write_temp_matrix(tmp_path: Path, update) -> Path:
    data = load_matrix()
    update(data)
    path = tmp_path / "matrix.json"
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return path


def test_real_matrix_resolves_and_local_verify_scope_is_opt_in():
    matrix, errors = checker.validate_matrix(MATRIX, ROOT)
    assert errors == []
    assert matrix is not None and len(matrix["scenarios"]) == 31

    assert "rtos-evidence" in run_local_verify.SCOPES
    assert run_local_verify.scope_includes("all", "rtos-evidence")
    assert run_local_verify.scope_includes("rtos-evidence", "rtos-evidence")
    assert not run_local_verify.scope_includes("software", "rtos-evidence")
    step, = run_local_verify.rtos_evidence_steps(ROOT, sys.executable)
    assert step.command[-1].endswith("check_rtos_evidence.py")


def test_missing_mutation_id_fails_on_temp_matrix(tmp_path: Path):
    def remove_mutation(data):
        next(row for row in data["scenarios"] if row["status"] == "HOST_COVERED")["mutations"] = ["missing_mutation"]

    matrix = write_temp_matrix(tmp_path, remove_mutation)
    _, errors = checker.validate_matrix(matrix, ROOT)
    assert any("mutation id not found: missing_mutation" in error for error in errors)


def test_blocked_row_without_reason_fails_on_temp_matrix(tmp_path: Path):
    def remove_reason(data):
        row = next(row for row in data["scenarios"] if row["status"] == "BLOCKED_NO_BOARD")
        row["blocked_reason"] = None

    matrix = write_temp_matrix(tmp_path, remove_reason)
    _, errors = checker.validate_matrix(matrix, ROOT)
    assert any("BLOCKED_NO_BOARD requires blocked_reason" in error for error in errors)


def test_hil_row_claiming_pass_fails_on_temp_matrix(tmp_path: Path):
    def claim_pass(data):
        row = next(row for row in data["scenarios"] if row["evidence_layer"] == "HIL")
        row["status"] = "PASS"

    matrix = write_temp_matrix(tmp_path, claim_pass)
    _, errors = checker.validate_matrix(matrix, ROOT)
    assert any("HIL evidence cannot claim PASS" in error for error in errors)


def test_unknown_host_test_fails_on_temp_matrix(tmp_path: Path):
    def replace_test(data):
        row = next(row for row in data["scenarios"] if row["host_tests"])
        row["host_tests"][0]["test_name"] = "test_not_in_manifest"

    matrix = write_temp_matrix(tmp_path, replace_test)
    _, errors = checker.validate_matrix(matrix, ROOT)
    assert any("host test not found" in error for error in errors)


def test_reports_attach_actual_results_and_run_manifest_smoke(tmp_path: Path):
    before = MATRIX.read_bytes()
    matrix = load_matrix()
    row = next(row for row in matrix["scenarios"] if row["status"] == "HOST_COVERED")
    host_test = row["host_tests"][0]
    mutation_id = row["mutations"][0]
    host_report = tmp_path / "host-test-report.json"
    mutation_report = tmp_path / "mutation-report.json"
    host_report.write_text(json.dumps({
        "manifest": str(ROOT / host_test["manifest"]),
        "results": [{"name": host_test["test_name"], "status": "PASS", "phase": "run", "returncode": 0}],
    }), encoding="utf-8")
    mutation_report.write_text(json.dumps({
        "exit_code": 0,
        "results": [{"id": mutation_id, "status": "KILLED", "duration": 0.01, "expected_killer": "test", "detail": ""}],
        "errors": [],
    }), encoding="utf-8")
    output = tmp_path / "run-manifest.json"

    exit_code = checker.main([
        "--repo-root", str(ROOT),
        "--host-test-report", str(host_report),
        "--mutation-report", str(mutation_report),
        "--run-manifest", str(output),
    ])
    assert exit_code == 0
    emitted = json.loads(output.read_text(encoding="utf-8"))
    assert emitted["git_sha"]
    assert isinstance(emitted["dirty"], bool)
    assert "sdkconfig" in {Path(item["path"]).name for item in emitted["config_sha256"]}
    assert emitted["signal_configuration"]["ESP32-S3"]["sample_rate_hz"] == 20000
    assert emitted["signal_configuration"]["ESP32-S3"]["channel_count"] == 4
    provenance = next(item for item in emitted["scenario_provenance"] if item["case_id"] == row["case_id"])
    assert provenance["src_sha"] == emitted["git_sha"]
    assert {Path(item["path"]).name for item in provenance["config_sha256"]} >= {"sdkconfig", "sdkconfig.defaults"}
    attached = next(item for item in emitted["observed_scenario_results"] if item["case_id"] == row["case_id"])
    assert attached["host_tests"][0]["status"] == "PASS"
    assert attached["mutations"][0]["status"] == "KILLED"
    assert MATRIX.read_bytes() == before
