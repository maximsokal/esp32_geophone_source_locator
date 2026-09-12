[CmdletBinding()]
param(
    [string]$BuildDir = "build-refactor-insights",

    [ValidateRange(1, 32)]
    [int]$Jobs = 2
)

$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$runIdf = Join-Path $PSScriptRoot "run_idf.ps1"
$profileSdkconfig = if ([IO.Path]::IsPathRooted($BuildDir)) {
    Join-Path $BuildDir "sdkconfig"
} else {
    Join-Path (Join-Path $repo $BuildDir) "sdkconfig"
}
if (-not (Test-Path -LiteralPath $runIdf -PathType Leaf)) {
    throw "Missing ESP-IDF helper: $runIdf"
}

Push-Location $repo
try {
    $env:GEOLOCATOR_INSIGHTS_PROFILE = "1"
    try {
        Write-Host "Building explicit ESP Insights compile/link profile (no flash)."
        & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $runIdf `
            -Action build -ProjectPath . -BuildDir $BuildDir -Sdkconfig $profileSdkconfig -Jobs $Jobs
        if ($LASTEXITCODE -ne 0) {
            throw "ESP Insights profile build failed with exit code $LASTEXITCODE."
        }
    } finally {
        Remove-Item Env:GEOLOCATOR_INSIGHTS_PROFILE -ErrorAction SilentlyContinue
    }
} finally {
    Pop-Location
}

Write-Host "INSIGHTS_PROFILE_RESULT: PASS"
