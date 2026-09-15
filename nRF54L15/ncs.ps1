param(
    [ValidateSet("build", "compare", "probe", "clean")]
    [string]$Action = "build"
)

$ErrorActionPreference = "Stop"
$NcsVersion = "v3.4.0"

$project = $PSScriptRoot
$app = Join-Path $project "zephyr"
$csConf = Join-Path $project "ncs\channel_sounding.conf"
$board = "xiao_nrf54l15/nrf54l15/cpuapp"
$buildRoot = Join-Path $env:USERPROFILE ".ncs-workspaces\esp32_geophone_source_locator"
$baseBuild = Join-Path $buildRoot "nrf54l15-base-v3.4.0"
$csBuild = Join-Path $buildRoot "nrf54l15-cs-v3.4.0"

function Require-Command([string]$Name, [string]$Hint) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name not found. $Hint"
    }
}

function Get-NcsVersionFromWorkspace([string]$TopDir) {
    $versionFile = Join-Path $TopDir "nrf\VERSION"
    if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
        throw "NCS VERSION file not found: $versionFile"
    }

    $parts = @{}
    foreach ($line in Get-Content -LiteralPath $versionFile) {
        if ($line -match '^\s*(VERSION_MAJOR|VERSION_MINOR|PATCHLEVEL)\s*=\s*(\d+)\s*$') {
            $parts[$matches[1]] = [int]$matches[2]
        }
    }
    foreach ($key in @("VERSION_MAJOR", "VERSION_MINOR", "PATCHLEVEL")) {
        if (-not $parts.ContainsKey($key)) {
            throw "Unable to parse $key from $versionFile"
        }
    }
    return "v$($parts.VERSION_MAJOR).$($parts.VERSION_MINOR).$($parts.PATCHLEVEL)"
}

function Get-NcsTopDir {
    Require-Command "west" "Open the Nordic toolchain terminal first: nrfutil sdk-manager toolchain launch --ncs-version $NcsVersion --terminal"

    # The diploma repository is outside the NCS west workspace, so `west topdir`
    # from the repository root is not a reliable primary discovery mechanism.
    # Nordic's activated toolchain environment normally exposes NRF_BASE and
    # ZEPHYR_BASE. Their parent directory is the NCS west workspace root.
    foreach ($base in @($env:NRF_BASE, $env:ZEPHYR_BASE)) {
        if ($base) {
            $candidate = Split-Path -Parent $base
            if (Test-Path -LiteralPath (Join-Path $candidate ".west") -PathType Container) {
                return $candidate
            }
        }
    }

    # Fallback for terminals whose current directory already is inside NCS.
    $topDir = (& west topdir 2>$null | Select-Object -First 1)
    if ($topDir) {
        $topDir = $topDir.Trim()
    }
    if (-not $topDir -or -not (Test-Path -LiteralPath (Join-Path $topDir ".west") -PathType Container)) {
        throw "NCS west workspace not detected. Launch the pinned NCS $NcsVersion toolchain terminal so NRF_BASE/ZEPHYR_BASE are set, then run this script again."
    }
    return $topDir
}

function Assert-NcsBaseline([string]$TopDir) {
    $actual = Get-NcsVersionFromWorkspace $TopDir
    if ($actual -ne $NcsVersion) {
        throw "MAX-45 is pinned to $NcsVersion, but active west workspace is $actual ($TopDir)."
    }

    $boardDir = Join-Path $TopDir "zephyr\boards\seeed\xiao_nrf54l15"
    if (-not (Test-Path -LiteralPath $boardDir -PathType Container)) {
        throw "Pinned NCS $NcsVersion does not expose the expected upstream XIAO board: $boardDir"
    }
}

function Invoke-WestBuild([string]$BuildDir, [bool]$WithChannelSounding) {
    $args = @(
        "build",
        "-p", "always",
        "-d", $BuildDir,
        "-b", $board,
        $app
    )
    if ($WithChannelSounding) {
        $args += @("--", "-DEXTRA_CONF_FILE=$csConf")
    }

    Write-Host ""
    Write-Host "west $($args -join ' ')"
    & west @args
    if ($LASTEXITCODE -ne 0) {
        throw "west build failed: $LASTEXITCODE"
    }
}

function Assert-ConfigLine([string[]]$Config, [string]$Line) {
    if ($Config -notcontains $Line) {
        throw "Resolved Kconfig missing: $Line"
    }
}

function Probe-CsBuild {
    $configPath = Join-Path $csBuild "zephyr\.config"
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        throw "CS build config not found: $configPath. Run .\nRF54L15\ncs.ps1 build first."
    }

    $config = Get-Content -LiteralPath $configPath
    foreach ($line in @(
        "CONFIG_BT=y",
        "CONFIG_BT_CENTRAL=y",
        "CONFIG_BT_PERIPHERAL=y",
        "CONFIG_BT_MAX_CONN=2",
        "CONFIG_BT_LL_SOFTDEVICE=y",
        "CONFIG_BT_LL_SOFTDEVICE_MULTIROLE=y",
        "CONFIG_BT_CHANNEL_SOUNDING=y",
        "CONFIG_BT_CTLR_CHANNEL_SOUNDING=y",
        "CONFIG_BT_CTLR_SDC_PERIPHERAL_COUNT=1",
        "CONFIG_BT_CTLR_SDC_CS_ROLE_BOTH=y",
        "CONFIG_BT_CTLR_SDC_CS_COUNT=1",
        "CONFIG_BT_CTLR_SDC_CS_MAX_ANTENNA_PATHS=1",
        "CONFIG_BT_CTLR_SDC_CS_NUM_ANTENNAS=1",
        "CONFIG_I2C_NRFX_TWIS_BUF_SIZE=128"
    )) {
        Assert-ConfigLine $config $line
    }

    foreach ($line in @(
        "# CONFIG_BT_CTLR_CHANNEL_SOUNDING_TEST is not set",
        "# CONFIG_BT_CTLR_SDC_CS_STEP_MODE3 is not set"
    )) {
        Assert-ConfigLine $config $line
    }

    $elf = Join-Path $csBuild "zephyr\zephyr.elf"
    if (-not (Test-Path -LiteralPath $elf -PathType Leaf)) {
        throw "CS ELF not found after build: $elf"
    }

    Write-Host "MAX-45 KCONFIG_PROBE: PASS" -ForegroundColor Green
}

function Show-Size([string]$Label, [string]$BuildDir) {
    Require-Command "arm-zephyr-eabi-size" "The active NCS toolchain does not expose arm-zephyr-eabi-size."
    $elf = Join-Path $BuildDir "zephyr\zephyr.elf"
    if (-not (Test-Path -LiteralPath $elf -PathType Leaf)) {
        throw "$Label ELF missing: $elf"
    }
    Write-Host ""
    Write-Host "===== $Label ====="
    & arm-zephyr-eabi-size $elf
    if ($LASTEXITCODE -ne 0) {
        throw "arm-zephyr-eabi-size failed for $Label"
    }
}

if ($Action -eq "clean") {
    Remove-Item -LiteralPath $baseBuild -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $csBuild -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "MAX-45 clean: PASS"
    exit 0
}

$topDir = Get-NcsTopDir
Assert-NcsBaseline $topDir
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

Push-Location $topDir
try {
    switch ($Action) {
        "build" {
            Invoke-WestBuild $csBuild $true
            Probe-CsBuild
        }
        "compare" {
            Invoke-WestBuild $baseBuild $false
            Invoke-WestBuild $csBuild $true
            Probe-CsBuild
            Show-Size "BASE" $baseBuild
            Show-Size "CHANNEL_SOUNDING" $csBuild
        }
        "probe" {
            Probe-CsBuild
            Show-Size "CHANNEL_SOUNDING" $csBuild
        }
    }
}
finally {
    Pop-Location
}

Write-Host "MAX-45 $Action: PASS" -ForegroundColor Green
