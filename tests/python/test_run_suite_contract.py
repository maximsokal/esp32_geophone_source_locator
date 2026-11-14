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
    assert '$baseBuildDir = Join-Path $repo ".test-build\\host-tests"' in source
    assert 'Join-Path $repo (".test-build\\host-tests-{0}" -f $suiteName)' in source
    assert '"--keep-going"' in source

    shared_host_branch = 'if ($Suite -eq "Host" -or $Suite -eq "All") {'
    assert shared_host_branch in source
    branch_start = source.index(shared_host_branch)
    python_start = source.index('if ($Suite -eq "All") {', branch_start)
    assert "Invoke-HostSuites" in source[branch_start:python_start]


def test_virtual_board_stress_native_builds_use_test_build():
    source = (
        REPO_ROOT / "tools" / "run_virtual_board_stress.ps1"
    ).read_text(encoding="utf-8")

    assert 'Join-Path $repo ".test-build\\virtual-board-stress"' in source
    assert 'Join-Path $repo ".test-build\\virtual-board-scheduler-stress"' in source


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


def test_all_suite_requires_emulator_build_then_low_memory_s1():
    source = (REPO_ROOT / "tools" / "run_suite.ps1").read_text(encoding="utf-8")
    branch = source[source.index('if ($Suite -eq "All") {\n    Invoke-SuiteStep -Name "ESP Unity build"'):]
    branch = branch[:branch.index('\n}\n')]
    build = 'Invoke-SuiteStep -Name "ESP emulator build"'
    smoke = 'Invoke-SuiteStep -Name "ESP32-S3 low-memory startup emulator"'
    assert source.count(build) == source.count(smoke) == 1
    assert branch.index('Invoke-SuiteStep -Name "ESP production build"') < branch.index(build) < branch.index(smoke)
    build_step = branch[branch.index(build):branch.index(smoke)]
    assert '-FilePath $powershell' in build_step
    assert '"-File", $runIdf' in build_step
    assert '"-Action", "build", "-Profile", "emulator"' in build_step
    smoke_step = branch[branch.index(smoke):branch.index('$results.Add')]
    assert '-FilePath "python"' in smoke_step
    assert '"tools/run_emulator_smoke.py", "--scenario", "S1", "--build-image", "--out-dir", ".test-build/emu-smoke/all-suite"' in smoke_step
    assert 'SKIPPED' not in build_step + smoke_step


def test_all_suite_builds_and_runs_the_recording_emulator_scenario_after_s1():
    source = (REPO_ROOT / "tools" / "run_suite.ps1").read_text(encoding="utf-8")
    s1 = 'Invoke-SuiteStep -Name "ESP32-S3 low-memory startup emulator"'
    build = 'Invoke-SuiteStep -Name "ESP emulator recording build"'
    run = 'Invoke-SuiteStep -Name "ESP32-S3 recording emulator (START-record-STOP, stack/heap)"'
    assert source.count(build) == source.count(run) == 1
    after_run = source.index("$results.Add", source.index(run))
    assert source.index(s1) < source.index(build) < source.index(run) < after_run
    build_step = source[source.index(build):source.index(run)]
    assert '"-Action", "build", "-Profile", "emulator-rec"' in build_step
    run_step = source[source.index(run):after_run]
    assert '"tools/run_emulator_smoke.py", "--scenario", "S10", "--build-rec-image"' in run_step
    assert "SKIPPED" not in build_step + run_step


def test_all_suite_uses_canonical_ncs_nrf_build_and_explicit_test_boundary():
    source = (REPO_ROOT / "tools" / "run_suite.ps1").read_text(encoding="utf-8")

    assert '$nrfNcsWrapper = Join-Path $repo "nRF54L15\\ncs.ps1"' in source
    assert 'Invoke-SuiteStep -Name "nRF54L15 NCS production build"' in source
    assert '"-File", $nrfNcsWrapper' in source
    assert '"-Action", "build"' in source
    assert 'function Add-NrfSoftwareTestBoundary' in source
    assert 'Invoke-SuiteStep -Name "nRF54L15 NCS bridge test build"' in source
    assert 'Invoke-SuiteStep -Name "nRF54L15 NCS Channel Sounding test build"' in source
    assert '"-Action", "ncs-test-build"' in source
    assert '"-Action", "ncs-cs-test-build"' in source
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
    assert "nRF54L15 TEST_RESULT: BLOCKED" in source
