[CmdletBinding()]
param(
    [ValidateSet("none", "address", "undefined", "address,undefined")]
    [string]$Sanitizers = "none",
    [string]$Compiler,
    [switch]$KeepGoing
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$runner = Join-Path $repo "tools\run_host_tests.py"

Write-Host "VIRTUAL_BOARD_RUNNER: SOFTWARE_ONLY"
Write-Host "VIRTUAL_BOARD_PHYSICAL_IO: NONE"
Write-Host "VIRTUAL_BOARD_TARGET_FLASH: NONE"
Write-Host "VIRTUAL_BOARD_TARGET_REQUIRED: NO"
Write-Host "VIRTUAL_BOARD_SERIAL_PORTS: NONE"
Write-Host "VIRTUAL_BOARD_HOST_MODEL: NATIVE_DISCRETE_TIME_MOCKS"

$arguments = @(
    $runner,
    "--manifest", (Join-Path $repo "tests\host\manifest.d\performance-acceptance-foundation.json"),
    "--only", "virtual_board_stress",
    "--only", "scheduler_replay_stress",
    "--build-dir", (Join-Path $repo "build\virtual-board-stress"),
    "--sanitizers", $Sanitizers
)
if ($Compiler) {
    $arguments += @("--compiler", $Compiler)
}
if ($KeepGoing) {
    $arguments += "--keep-going"
}

& python @arguments
$exitCode = $LASTEXITCODE
if ($exitCode -eq 0 -or $KeepGoing) {
    $replayArguments = @(
        $runner,
        "--manifest", (Join-Path $repo "tests\host\manifest.json"),
        "--only", "scheduler_replay_stress",
        "--build-dir", (Join-Path $repo "build\virtual-board-scheduler-stress"),
        "--sanitizers", $Sanitizers
    )
    if ($Compiler) {
        $replayArguments += @("--compiler", $Compiler)
    }
    if ($KeepGoing) {
        $replayArguments += "--keep-going"
    }

    & python @replayArguments
    if ($exitCode -eq 0) {
        $exitCode = $LASTEXITCODE
    }
}
if ($exitCode -eq 0) {
    Write-Host "VIRTUAL_BOARD_STRESS_RUNNER_RESULT: PASS"
} else {
    Write-Host "VIRTUAL_BOARD_STRESS_RUNNER_RESULT: FAIL"
}
exit $exitCode
