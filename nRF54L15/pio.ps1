param(
    [ValidateSet("build", "clean", "upload", "monitor", "test")]
    [string]$Action = "build"
)

$ErrorActionPreference = "Stop"

$project = $PSScriptRoot
$pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"

if (-not (Test-Path $pio)) {
    throw "PlatformIO not found: $pio"
}

# ------------------------------------------------------------
# IMPORTANT
#
# Zephyr/PlatformIO currently has problems with spaces in paths.
#
# Source project stays in:
# E:\Beetroot Academy\...\nRF54L15
#
# Source is exposed through SUBST (P:\ etc).
#
# ALL generated PlatformIO files are placed separately in a
# path without spaces:
# C:\Users\Maxim\.platformio-workspaces\nrf54l15
# ------------------------------------------------------------

$workspace = Join-Path $env:USERPROFILE ".platformio-workspaces\nrf54l15"
$buildDir  = Join-Path $workspace "build"
$libdeps   = Join-Path $workspace "libdeps"

New-Item -ItemType Directory -Force -Path $workspace | Out-Null

# Official PlatformIO directory overrides.
$env:PLATFORMIO_WORKSPACE_DIR = $workspace
$env:PLATFORMIO_BUILD_DIR     = $buildDir
$env:PLATFORMIO_LIBDEPS_DIR   = $libdeps

# Find unused drive letter.
$drive = $null

foreach ($letter in @("P","Q","R","S","T","U","V","W","X","Y","Z")) {
    if (-not (Get-PSDrive -Name $letter -ErrorAction SilentlyContinue)) {
        $drive = "${letter}:"
        break
    }
}

if (-not $drive) {
    throw "No free drive letter P:..Z:"
}

Write-Host ""
Write-Host "========================================"
Write-Host " nRF54L15 PlatformIO"
Write-Host "========================================"
Write-Host "Real project : $project"
Write-Host "Source alias : $drive\"
Write-Host "Workspace    : $workspace"
Write-Host "Build dir    : $buildDir"
Write-Host "Action       : $Action"
Write-Host ""

# Create no-space alias to actual project.
cmd.exe /d /c ('subst {0} "{1}"' -f $drive, $project)

if ($LASTEXITCODE -ne 0) {
    throw "subst failed"
}

$mappedProject = "$drive\"
$exitCode = 1
$actionStatus = "SUCCESS"
$preserveExitCode = $false

try {
    Push-Location $mappedProject

    try {
        Write-Host "PlatformIO:"
        & $pio --version

        if ($LASTEXITCODE -ne 0) {
            throw "pio --version failed"
        }

        Write-Host ""
        Write-Host "Current project path:"
        Write-Host (Get-Location).Path

        Write-Host ""
        Write-Host "Generated files path:"
        Write-Host $workspace
        Write-Host ""

        switch ($Action) {

            "build" {
                & $pio run `
                    --environment seeed-xiao-nrf54l15
            }

            "clean" {
                & $pio run `
                    --environment seeed-xiao-nrf54l15 `
                    --target clean
            }

            "upload" {
                & $pio run `
                    --environment seeed-xiao-nrf54l15 `
                    --target upload
            }

            "monitor" {
                & $pio device monitor `
                    --environment seeed-xiao-nrf54l15
            }

            "test" {
                $testDirectory = Join-Path $mappedProject "tests\platformio_native"
                $softwareSuites = @()
                if (Test-Path -LiteralPath $testDirectory -PathType Container) {
                    $softwareSuites = @(
                        Get-ChildItem -LiteralPath $testDirectory -Directory -Filter "test_sw_*" |
                            Sort-Object Name
                    )
                }

                if ($softwareSuites.Count -eq 0) {
                    $actionStatus = "SKIPPED"
                    Write-Host "nRF54L15 TEST_RESULT: SKIPPED (no qualified software/native tests/platformio_native/test_sw_* suite exists yet; physical/HIL suites are intentionally excluded from this action)." -ForegroundColor Yellow
                } elseif (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
                    $actionStatus = "BLOCKED"
                    $exitCode = 2
                    $preserveExitCode = $true
                    Write-Host "nRF54L15 TEST_RESULT: BLOCKED (the native PlatformIO runner requires gcc, but no gcc executable is available in PATH; install a host GCC toolchain or run the repository MSVC host manifest)." -ForegroundColor Yellow
                } else {
                    # This automatic entry point is reserved for suites explicitly
                    # qualified as software/native. --without-uploading alone does
                    # not make an embedded PlatformIO suite hardware-free, so
                    # physical/HIL suites must use a separate explicit runner.
                    & $pio test `
                        --environment native `
                        --filter "test_sw_*" `
                        --without-uploading
                }
            }
        }

        if (-not $preserveExitCode) {
            $exitCode = $LASTEXITCODE
        }
    }
    finally {
        Pop-Location
    }
}
finally {
    cmd.exe /d /c "subst $drive /D" | Out-Null
}

Write-Host ""

if ($exitCode -ne 0) {
    Write-Host "========================================"
    if ($actionStatus -eq "BLOCKED") {
        Write-Host " nRF54L15 BLOCKED: $exitCode"
    } else {
        Write-Host " nRF54L15 FAILED: $exitCode"
    }
    Write-Host "========================================"

    exit $exitCode
}

Write-Host "========================================"
Write-Host (" nRF54L15 {0}" -f $actionStatus)
Write-Host "========================================"

exit 0
