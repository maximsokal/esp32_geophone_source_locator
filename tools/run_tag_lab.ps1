[CmdletBinding()]
param(
    # Run only the Tag Lab vitest suites instead of starting the page.
    [switch]$Tests
)

# Development-only launcher for the webcam AprilTag lab (no ESP32, no firmware, no board API).
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$webui = Join-Path $repo "webui"

function Find-Node {
    $cmd = Get-Command node -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    # nvm-windows installs that were never activated with `nvm use` keep node.exe under the nvm root.
    $roots = @($env:NVM_HOME, (Join-Path $env:LOCALAPPDATA "nvm")) | Where-Object { $_ -and (Test-Path $_) }
    foreach ($root in $roots) {
        $found = Get-ChildItem -Path $root -Directory -Filter "v*" -ErrorAction SilentlyContinue |
            Sort-Object { [version]($_.Name.TrimStart("v")) } -Descending |
            ForEach-Object { Join-Path $_.FullName "node.exe" } |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($found) { return $found }
    }
    throw "node.exe was not found on PATH or under the nvm root."
}

$node = Find-Node
Write-Host "[tag-lab] starting (node: $node)"
Set-Location $webui

if ($Tests) {
    $logRoot = Join-Path $repo "logs\tag-lab\test-runs"
    New-Item -ItemType Directory -Force $logRoot | Out-Null
    $out = Join-Path $logRoot ("vitest_{0}.log" -f (Get-Date -Format "yyyy-MM-dd_HHmmss"))
    $vitest = Join-Path $webui "node_modules\vitest\vitest.mjs"
    & $node $vitest run tests/tag-lab --exclude '**/*.spec.ts' tests/xr/fiducialTagDetector.test.ts tests/xr/tagDetectorClient.test.ts tests/xr/tagPipelineEndToEnd.test.ts tests/xr/tagSheetPatterns.test.ts *>&1 | Tee-Object -FilePath $out
    $code = $LASTEXITCODE
    Write-Host "[tag-lab] vitest exit code $code; log $out"
    exit $code
}

try {
    & $node (Join-Path $webui "scripts\run-tag-lab.mjs")
    exit $LASTEXITCODE
} catch {
    Write-Host "[tag-lab] ERROR $($_.Exception.Message)"
    exit 1
}
