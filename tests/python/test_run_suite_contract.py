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

    shared_host_branch = 'if ($Suite -eq "Host" -or $Suite -eq "All") {'
    assert shared_host_branch in source
    branch_start = source.index(shared_host_branch)
    python_start = source.index('if ($Suite -eq "All") {', branch_start)
    assert "Invoke-HostSuites" in source[branch_start:python_start]


def test_all_suite_surfaces_only_current_host_failures_in_final_report():
    source = (REPO_ROOT / "tools" / "run_suite.ps1").read_text(encoding="utf-8")

    assert "function Add-HostFailureDetails" in source
    assert "function Compress-DiagnosticText" in source
    assert '"host-test-report.json"' in source
    assert "Remove-Item -LiteralPath $baseReport -Force -ErrorAction SilentlyContinue" in source
    assert "Remove-Item -LiteralPath $reportPath -Force -ErrorAction SilentlyContinue" in source
    assert "$report.results" in source
    assert '$testResult.status -ne "FAIL"' in source
    assert "$report.compiler_description" in source
    assert "$testResult.stderr" in source
    assert "$testResult.stdout" in source
    assert '$testResult.error' in source
    assert 'Write-Host "HOST_FAILURE_DETAILS:"' in source


def test_all_suite_keeps_hardware_hil_out_of_automatic_run():
    source = (REPO_ROOT / "tools" / "run_suite.ps1").read_text(encoding="utf-8")

    assert 'Name = "ESP Unity HIL (COM4+COM6)"' in source
    assert 'Status = "SKIPPED"' in source
    assert 'HIL_RESULT: SKIPPED' in source
    assert 'run_unity_hil' not in source.lower()
    assert '"-Action", "flash"' not in source


def test_all_suite_uses_canonical_ncs_nrf_build_and_explicit_test_boundary():
    source = (REPO_ROOT / "tools" / "run_suite.ps1").read_text(encoding="utf-8")

    assert '$nrfNcsWrapper = Join-Path $repo "nRF54L15\\ncs.ps1"' in source
    assert 'Invoke-SuiteStep -Name "nRF54L15 NCS production build"' in source
    assert '"-File", $nrfNcsWrapper' in source
    assert '"-Action", "build"' in source
    assert 'function Add-NrfSoftwareTestBoundary' in source
    assert '-Filter "test_sw_*"' in source
    assert 'PlatformIO compatibility tests are excluded' in source
    assert 'Status = "SKIPPED"' in source
    assert 'nRF54L15\\pio.ps1' not in source


def test_platformio_wrapper_remains_compatibility_only_and_no_upload_test_is_not_physical_proof():
    source = (REPO_ROOT / "nRF54L15" / "pio.ps1").read_text(encoding="utf-8")

    assert 'ValidateSet("build", "clean", "upload", "monitor", "test")' in source
    assert '"test" {' in source
    assert '-Filter "test_sw_*"' in source
    assert '--filter "test_sw_*"' in source
    assert "--without-uploading" in source
    assert "physical/HIL suites must use a separate explicit runner" in source
    assert "nRF54L15 TEST_RESULT: SKIPPED" in source
