import json
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def test_host_c_vscode_task_routes_through_complete_host_suite():
    tasks = json.loads((REPO_ROOT / ".vscode" / "tasks.json").read_text(encoding="utf-8"))
    host_task = next(task for task in tasks["tasks"] if task.get("label") == "Tests: Host C")

    assert host_task["command"] == "powershell"
    args = host_task["args"]
    assert "${workspaceFolder}/tools/run_suite.ps1" in args
    assert "-Suite" in args
    assert args[args.index("-Suite") + 1] == "Host"
    assert all("run_host_tests.py" not in str(arg) for arg in args)


def test_host_suite_mode_reuses_base_and_manifest_fragment_enumerator():
    source = (REPO_ROOT / "tools" / "run_suite.ps1").read_text(encoding="utf-8")

    assert '[ValidateSet("All", "Web", "Host")]' in source
    assert 'if ($Suite -eq "Host" -or $Suite -eq "All") {' in source
    assert "Invoke-HostSuites" in source
    assert 'Join-Path $repo "tests\\host\\manifest.d"' in source
    assert 'Get-ChildItem -LiteralPath $manifestDirectory -Filter "*.json" -File' in source
    assert 'Add-HostFailureDetails -SuiteName $stepName -ReportPath $reportPath' in source
