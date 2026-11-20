[CmdletBinding()]
param(
    # Run only the Phone Lab vitest suites instead of starting the server.
    [switch]$Tests,
    # Use a self-signed certificate instead of one signed by the project's Device CA.
    [switch]$SelfSigned
)

# Development-only: serves the real WebUI to a phone on the home Wi-Fi (HTTPS + fake ESP32 API + telemetry) and prints a QR.
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$webui = Join-Path $repo "webui"

function Find-Node {
    $cmd = Get-Command node -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
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
Write-Host "[phone-lab] starting (node: $node)"
Set-Location $webui
if ($SelfSigned) { $env:PHONE_LAB_SELFSIGNED = "1" }

if ($Tests) {
    $logRoot = Join-Path $repo "logs\phone-lab\test-runs"
    New-Item -ItemType Directory -Force $logRoot | Out-Null
    $out = Join-Path $logRoot ("vitest_{0}.log" -f (Get-Date -Format "yyyy-MM-dd_HHmmss"))
    & $node (Join-Path $webui "node_modules\vitest\vitest.mjs") run tests/phone-lab --exclude '**/*.spec.ts' *>&1 | Tee-Object -FilePath $out
    $code = $LASTEXITCODE
    Write-Host "[phone-lab] vitest exit code $code; log $out"
    exit $code
}

try {
    & $node (Join-Path $webui "scripts\run-phone-lab.mjs")
    exit $LASTEXITCODE
} catch {
    Write-Host "[phone-lab] ERROR $($_.Exception.Message)"
    exit 1
}
