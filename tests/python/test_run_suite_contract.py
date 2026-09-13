from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def test_all_suite_runs_base_and_every_host_manifest_fragment():
    source = (REPO_ROOT / "tools" / "run_suite.ps1").read_text(encoding="utf-8")

    assert "function Invoke-HostSuites" in source
    assert 'Invoke-SuiteStep -Name "Host C base"' in source
    assert '"tests\\host\\manifest.d"' in source
    assert 'Get-ChildItem -LiteralPath $manifestDirectory -Filter "*.json" -File' in source
    assert 'Sort-Object Name' in source
    assert '"--manifest"' in source
    assert '"--build-dir"' in source
    assert '"--keep-going"' in source
    assert 'Invoke-HostSuites' in source[source.index('if ($Suite -eq "All")'):]


def test_all_suite_surfaces_only_current_host_failures_in_final_report():
    source = (REPO_ROOT / "tools" / "run_suite.ps1").read_text(encoding="utf-8")

    assert "function Add-HostFailureDetails" in source
    assert '"host-test-report.json"' in source
    assert "Remove-Item -LiteralPath $baseReport -Force -ErrorAction SilentlyContinue" in source
    assert "Remove-Item -LiteralPath $reportPath -Force -ErrorAction SilentlyContinue" in source
    assert "$report.results" in source
    assert '$testResult.status -ne "FAIL"' in source
    assert 'Write-Host "HOST_FAILURE_DETAILS:"' in source
    assert "test={1} phase={2} exit={3}" in source


def test_all_suite_keeps_hardware_hil_out_of_automatic_run():
    source = (REPO_ROOT / "tools" / "run_suite.ps1").read_text(encoding="utf-8")

    assert 'Name = "ESP Unity HIL (COM4+COM6)"' in source
    assert 'Status = "SKIPPED"' in source
    assert 'HIL_RESULT: SKIPPED' in source
    assert 'run_unity_hil' not in source.lower()
