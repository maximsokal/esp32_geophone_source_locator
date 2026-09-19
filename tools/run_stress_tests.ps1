[CmdletBinding()]
param(
    [ValidateSet("none", "address", "undefined", "address,undefined")]
    [string]$Sanitizers = "none",
    [string]$Compiler,
    [switch]$KeepGoing
)

# Compatibility alias: the canonical entry point is run_virtual_board_stress.ps1
# (software-only virtual_board_stress + scheduler_replay_stress). Kept as a thin
# delegator, not a copy, so the two can never drift out of sync again.
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$canonical = Join-Path $repo "tools\run_virtual_board_stress.ps1"

$arguments = @{ Sanitizers = $Sanitizers }
if ($Compiler) {
    $arguments["Compiler"] = $Compiler
}
if ($KeepGoing) {
    $arguments["KeepGoing"] = $true
}

& $canonical @arguments
exit $LASTEXITCODE
