[CmdletBinding()]
param(
    [ValidateSet("build", "flash", "monitor", "cs-proof", "compare", "probe", "clean")]
    [string]$Action = "build",

    [string]$Port,

    [ValidateRange(1200, 3000000)]
    [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"
$NcsVersion = "v3.4.0"

$project = $PSScriptRoot
$app = Join-Path $project "zephyr"
$csConf = Join-Path $project "ncs\channel_sounding.conf"
$board = "xiao_nrf54l15/nrf54l15/cpuapp"
$buildRoot = Join-Path $env:USERPROFILE ".ncs-workspaces\esp32_geophone_source_locator"
$productionBuild = Join-Path $buildRoot "nrf54l15-production-v3.4.0"
$csProofBuild = Join-Path $buildRoot "nrf54l15-cs-proof-v3.4.0"
$legacyBaseBuild = Join-Path $buildRoot "nrf54l15-base-v3.4.0"
$legacyCsBuild = Join-Path $buildRoot "nrf54l15-cs-v3.4.0"

function Require-Command([string]$Name, [string]$Hint) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name not found. $Hint"
    }
}

function Invoke-InPinnedToolchain {
    Require-Command "nrfutil" "Install nRF Util and NCS $NcsVersion with sdk-manager first."

    $childArgs = @(
        "sdk-manager", "toolchain", "launch",
        "--ncs-version", $NcsVersion,
        "--",
        "powershell.exe",
        "-NoLogo",
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $PSCommandPath,
        "-Action", $Action
    )

    if ($Port) {
        $childArgs += @("-Port", $Port)
    }
    if ($Baud -ne 115200) {
        $childArgs += @("-Baud", [string]$Baud)
    }

    Write-Host "Launching pinned Nordic toolchain $NcsVersion..." -ForegroundColor Cyan
    & nrfutil @childArgs
    $exitCode = $LASTEXITCODE
    if ($null -eq $exitCode) {
        $exitCode = 0
    }
    exit $exitCode
}

function New-NoSpaceProjectAlias([string]$ProjectPath) {
    if ($env:OS -ne "Windows_NT" -or $ProjectPath -notmatch '\s') {
        return $null
    }

    Require-Command "cmd.exe" "Windows command processor is required for SUBST."

    $drive = $null
    foreach ($letter in @("P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z")) {
        if (-not (Get-PSDrive -Name $letter -ErrorAction SilentlyContinue)) {
            $drive = "${letter}:"
            break
        }
    }

    if (-not $drive) {
        throw "No free drive letter P:..Z: for the NCS no-space source alias."
    }

    & cmd.exe /d /c ('subst {0} "{1}"' -f $drive, $ProjectPath) | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "subst failed for NCS project alias: $drive -> $ProjectPath"
    }

    return $drive
}

function Remove-NoSpaceProjectAlias([string]$Drive) {
    if (-not $Drive) {
        return
    }

    & cmd.exe /d /c "subst $Drive /D" | Out-Null
}

function Test-NcsWorkspace([string]$Candidate) {
    if (-not $Candidate) {
        return $false
    }

    return (
        (Test-Path -LiteralPath (Join-Path $Candidate ".west") -PathType Container) -and
        (Test-Path -LiteralPath (Join-Path $Candidate "nrf\VERSION") -PathType Leaf)
    )
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

function Get-SdkManagerDefaultInstallRoot {
    if ($env:OS -eq "Windows_NT") {
        $systemDrive = if ($env:SystemDrive) { $env:SystemDrive } else { "C:" }
        return Join-Path $systemDrive "ncs"
    }

    if ($IsMacOS) {
        return "/opt/nordic/ncs"
    }

    return Join-Path $HOME "ncs"
}

function Invoke-SdkManager([string[]]$SdkManagerArgs) {
    Require-Command "nrfutil" "Install nRF Util and its sdk-manager command first."

    $originalHome = $env:NRFUTIL_HOME
    $redirectHome = $false

    # A launched Nordic toolchain can point NRFUTIL_HOME at a locked command
    # set. sdk-manager metadata is user-scoped, so temporarily use the normal
    # user nrfutil home without mutating the toolchain lock.
    if ($originalHome) {
        $lockPath = Join-Path $originalHome "locked"
        if (Test-Path -LiteralPath $lockPath -PathType Leaf) {
            $userHome = Join-Path $env:USERPROFILE ".nrfutil"
            New-Item -ItemType Directory -Force -Path $userHome | Out-Null
            $env:NRFUTIL_HOME = $userHome
            $redirectHome = $true
        }
    }

    try {
        $output = @(& nrfutil sdk-manager @SdkManagerArgs 2>&1)
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0) {
            $details = ($output | Out-String).Trim()
            throw "nrfutil sdk-manager $($SdkManagerArgs -join ' ') failed with exit code $exitCode. $details"
        }
        return $output
    }
    finally {
        if ($redirectHome) {
            $env:NRFUTIL_HOME = $originalHome
        }
    }
}

function Get-SdkManagerInstallRoot {
    $configOutput = @(Invoke-SdkManager @("config", "show"))

    foreach ($line in $configOutput) {
        if ($line -match '^\s*install-dir:\s*(.+?)\s*$') {
            $configured = $matches[1].Trim().Trim('"').Trim("'")
            if ($configured -and $configured -ne "unset") {
                return $configured
            }
            break
        }
    }

    return Get-SdkManagerDefaultInstallRoot
}

function Get-SdkManagerSdkDirectory {
    $listOutput = @(Invoke-SdkManager @("list", "--all-fields"))
    $versionPattern = [regex]::Escape($NcsVersion)

    foreach ($line in $listOutput) {
        # sdk-manager 1.16.x columns are:
        # SDK Type | SDK Version | SDK Status | SDK Directory | Toolchain Status
        if (
            $line -match
                "^\s*nrf\s+$versionPattern\s+Installed\s+(.+?)\s+(Installed|Not\s+Installed)\s*$"
        ) {
            return $matches[1].Trim()
        }
    }

    throw "NCS $NcsVersion SDK is not installed according to 'nrfutil sdk-manager list --all-fields'."
}

function Get-NcsTopDirFromSdkManager {
    $installRoot = Get-SdkManagerInstallRoot
    $sdkDirectory = Get-SdkManagerSdkDirectory
    $candidate = if ([System.IO.Path]::IsPathRooted($sdkDirectory)) {
        $sdkDirectory
    }
    else {
        Join-Path $installRoot $sdkDirectory
    }

    if (-not (Test-NcsWorkspace $candidate)) {
        throw "nrfutil reports NCS $NcsVersion at '$candidate', but that directory is not a west workspace with nrf\VERSION."
    }
    return $candidate
}

function Get-NcsTopDir {
    Require-Command "west" "The pinned Nordic toolchain did not expose west."

    foreach ($base in @($env:NRF_BASE, $env:ZEPHYR_BASE)) {
        if ($base) {
            $candidate = Split-Path -Parent $base
            if (Test-NcsWorkspace $candidate) {
                return $candidate
            }
        }
    }

    $topDir = $null
    try {
        $topDirOutput = @(& west topdir 2>$null)
        if ($LASTEXITCODE -eq 0 -and $topDirOutput.Count -gt 0) {
            $topDir = $topDirOutput[0]
        }
    }
    catch {
        $topDir = $null
    }

    if ($topDir) {
        $topDir = $topDir.Trim()
        if (Test-NcsWorkspace $topDir) {
            return $topDir
        }
    }

    return Get-NcsTopDirFromSdkManager
}

function Assert-NcsBaseline([string]$TopDir) {
    $actual = Get-NcsVersionFromWorkspace $TopDir
    if ($actual -ne $NcsVersion) {
        throw "nRF54L15 production is pinned to $NcsVersion, but active west workspace is $actual ($TopDir)."
    }

    $boardDir = Join-Path $TopDir "zephyr\boards\seeed\xiao_nrf54l15"
    if (-not (Test-Path -LiteralPath $boardDir -PathType Container)) {
        throw "Pinned NCS $NcsVersion does not expose the expected upstream XIAO board: $boardDir"
    }
}

function Invoke-WestBuild([string]$BuildDir, [bool]$WithChannelSoundingProof) {
    $args = @(
        "build",
        "-p", "always",
        "-d", $BuildDir,
        "-b", $board,
        $app
    )
    if ($WithChannelSoundingProof) {
        $args += @("--", "-DEXTRA_CONF_FILE=$csConf")
    }

    Write-Host ""
    Write-Host "west $($args -join ' ')"
    & west @args
    if ($LASTEXITCODE -ne 0) {
        throw "west build failed: $LASTEXITCODE"
    }
}

function Invoke-WestFlash([string]$BuildDir) {
    Write-Host ""
    Write-Host "west flash -d $BuildDir" -ForegroundColor Cyan
    & west flash -d $BuildDir
    if ($LASTEXITCODE -ne 0) {
        throw "west flash failed: $LASTEXITCODE"
    }
}

function Assert-ConfigLine([string[]]$Config, [string]$Line) {
    if ($Config -notcontains $Line) {
        throw "Resolved Kconfig missing: $Line"
    }
}

function Get-ZephyrAppArtifactDir([string]$BuildDir) {
    # NCS v3.4.0 runs this application through sysbuild. The outer
    # <build>/zephyr directory belongs to sysbuild itself; the actual app
    # writes .config and zephyr.elf under <build>/zephyr/zephyr.
    $sysbuildAppDir = Join-Path $BuildDir "zephyr\zephyr"
    if (Test-Path -LiteralPath $sysbuildAppDir -PathType Container) {
        return $sysbuildAppDir
    }

    return Join-Path $BuildDir "zephyr"
}

function Probe-ProductionBuild {
    $appZephyrDir = Get-ZephyrAppArtifactDir $productionBuild
    $configPath = Join-Path $appZephyrDir ".config"
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        throw "Production build config not found: $configPath. Run .\nRF54L15\ncs.ps1 build first."
    }

    $config = Get-Content -LiteralPath $configPath
    foreach ($line in @(
        "CONFIG_GPIO=y",
        "CONFIG_I2C=y",
        "CONFIG_I2C_TARGET=y",
        "CONFIG_I2C_NRFX_TWIS_BUF_SIZE=128",
        "CONFIG_HWINFO=y"
    )) {
        Assert-ConfigLine $config $line
    }

    $elf = Join-Path $appZephyrDir "zephyr.elf"
    if (-not (Test-Path -LiteralPath $elf -PathType Leaf)) {
        throw "Production ELF not found after build: $elf"
    }

    Write-Host "nRF54L15 PRODUCTION_PROBE: PASS" -ForegroundColor Green
}

function Probe-CsBuild {
    $appZephyrDir = Get-ZephyrAppArtifactDir $csProofBuild
    $configPath = Join-Path $appZephyrDir ".config"
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        throw "CS proof config not found: $configPath. Run .\nRF54L15\ncs.ps1 cs-proof first."
    }

    $config = Get-Content -LiteralPath $configPath
    foreach ($line in @(
        "CONFIG_BT=y",
        "CONFIG_BT_CENTRAL=y",
        "CONFIG_BT_PERIPHERAL=y",
        "CONFIG_BT_MAX_CONN=1",
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

    $elf = Join-Path $appZephyrDir "zephyr.elf"
    if (-not (Test-Path -LiteralPath $elf -PathType Leaf)) {
        throw "CS proof ELF not found after build: $elf"
    }

    Write-Host "MAX-45 KCONFIG_PROBE: PASS" -ForegroundColor Green
}

function Show-Size([string]$Label, [string]$BuildDir) {
    Require-Command "arm-zephyr-eabi-size" "The active NCS toolchain does not expose arm-zephyr-eabi-size."
    $appZephyrDir = Get-ZephyrAppArtifactDir $BuildDir
    $elf = Join-Path $appZephyrDir "zephyr.elf"
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

function Invoke-SerialMonitor {
    if ([string]::IsNullOrWhiteSpace($Port)) {
        throw "monitor requires -Port, for example: .\nRF54L15\ncs.ps1 monitor -Port COM7"
    }

    $serial = [System.IO.Ports.SerialPort]::new(
        $Port,
        $Baud,
        [System.IO.Ports.Parity]::None,
        8,
        [System.IO.Ports.StopBits]::One
    )
    $serial.ReadTimeout = 250
    $serial.DtrEnable = $true

    Write-Host "nRF54L15 serial monitor: $Port @ $Baud baud (Ctrl+C to stop)" -ForegroundColor Cyan
    try {
        $serial.Open()
        while ($true) {
            try {
                $line = $serial.ReadLine()
                Write-Host $line
            }
            catch [System.TimeoutException] {
                # Periodic timeout keeps Ctrl+C responsive without busy looping.
            }
        }
    }
    finally {
        if ($serial.IsOpen) {
            $serial.Close()
        }
        $serial.Dispose()
    }
}

if ($Action -eq "clean") {
    foreach ($path in @($productionBuild, $csProofBuild, $legacyBaseBuild, $legacyCsBuild)) {
        Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "nRF54L15 clean: PASS" -ForegroundColor Green
    exit 0
}

if ($Action -eq "monitor") {
    Invoke-SerialMonitor
    exit 0
}

# Build/flash/proof commands are safe to call from an ordinary PowerShell.
# If west is absent, re-enter this script through Nordic's pinned toolchain.
if (-not (Get-Command "west" -ErrorAction SilentlyContinue)) {
    Invoke-InPinnedToolchain
}

$topDir = Get-NcsTopDir
Assert-NcsBaseline $topDir
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

# Zephyr does not support application/source paths containing spaces reliably.
# Keep the checkout untouched and expose nRF54L15 through a temporary SUBST
# drive on Windows. Build output itself stays outside the repository.
$sourceAliasDrive = New-NoSpaceProjectAlias $project
$buildProject = $project
if ($sourceAliasDrive) {
    $buildProject = "$sourceAliasDrive\"
    $app = Join-Path $buildProject "zephyr"
    $csConf = Join-Path $buildProject "ncs\channel_sounding.conf"
    Write-Host "NCS source alias: $buildProject -> $project"
}

$zephyrBase = Join-Path $topDir "zephyr"
$originalZephyrBase = $env:ZEPHYR_BASE
$env:ZEPHYR_BASE = $zephyrBase

Push-Location $buildProject
try {
    switch ($Action) {
        "build" {
            Invoke-WestBuild $productionBuild $false
            Probe-ProductionBuild
            Write-Host "nRF54L15 production build: PASS" -ForegroundColor Green
        }
        "flash" {
            Invoke-WestBuild $productionBuild $false
            Probe-ProductionBuild
            Invoke-WestFlash $productionBuild
            Write-Host "nRF54L15 production flash: PASS" -ForegroundColor Green
        }
        "cs-proof" {
            Invoke-WestBuild $csProofBuild $true
            Probe-CsBuild
            Write-Host "MAX-45 cs-proof: PASS" -ForegroundColor Green
        }
        "compare" {
            Invoke-WestBuild $productionBuild $false
            Probe-ProductionBuild
            Invoke-WestBuild $csProofBuild $true
            Probe-CsBuild
            Show-Size "PRODUCTION" $productionBuild
            Show-Size "CHANNEL_SOUNDING_PROOF" $csProofBuild
            Write-Host "MAX-45 compare: PASS" -ForegroundColor Green
        }
        "probe" {
            Probe-CsBuild
            Show-Size "CHANNEL_SOUNDING_PROOF" $csProofBuild
            Write-Host "MAX-45 probe: PASS" -ForegroundColor Green
        }
    }
}
finally {
    Pop-Location
    Remove-NoSpaceProjectAlias $sourceAliasDrive
    if ($null -eq $originalZephyrBase) {
        Remove-Item Env:ZEPHYR_BASE -ErrorAction SilentlyContinue
    }
    else {
        $env:ZEPHYR_BASE = $originalZephyrBase
    }
}
