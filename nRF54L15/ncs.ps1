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

    # Nordic's launched toolchain intentionally points NRFUTIL_HOME at a locked
    # toolchain-local command set. sdk-manager is a user command, so use the
    # normal user-scoped nrfutil home for metadata queries without modifying or
    # deleting the toolchain's lock file.
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
    Require-Command "west" "Open the Nordic toolchain terminal first: nrfutil sdk-manager toolchain launch --ncs-version $NcsVersion --terminal"

    # Fast path for environments that explicitly expose the SDK module roots.
    # Nordic's sdk-manager toolchain environment does not guarantee these
    # variables, so discovery must not depend on them being present.
    foreach ($base in @($env:NRF_BASE, $env:ZEPHYR_BASE)) {
        if ($base) {
            $candidate = Split-Path -Parent $base
            if (Test-NcsWorkspace $candidate) {
                return $candidate
            }
        }
    }

    # Works when the current directory already belongs to an NCS west workspace.
    # Windows PowerShell can promote native stderr to a terminating error when
    # ErrorActionPreference is Stop, so failure here must be treated as a normal
    # probe miss and fall through to sdk-manager metadata discovery.
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

    # The diploma checkout normally lives outside the NCS workspace. Nordic's
    # supported sdk-manager metadata supplies both the configured install root
    # and the SDK Directory column for the exact installed version.
    return Get-NcsTopDirFromSdkManager
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

function Get-ZephyrAppArtifactDir([string]$BuildDir) {
    # NCS v3.4.0 runs this application through sysbuild. The outer
    # <build>/zephyr directory belongs to sysbuild itself; the actual Zephyr
    # application writes .config and zephyr.elf under <build>/zephyr/zephyr.
    # Keep a non-sysbuild fallback so the wrapper remains usable if west is
    # explicitly configured without sysbuild in a future toolchain.
    $sysbuildAppDir = Join-Path $BuildDir "zephyr\zephyr"
    if (Test-Path -LiteralPath $sysbuildAppDir -PathType Container) {
        return $sysbuildAppDir
    }

    return Join-Path $BuildDir "zephyr"
}

function Probe-CsBuild {
    $appZephyrDir = Get-ZephyrAppArtifactDir $csBuild
    $configPath = Join-Path $appZephyrDir ".config"
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        throw "CS build config not found: $configPath. Run .\nRF54L15\ncs.ps1 build first."
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
        throw "CS ELF not found after build: $elf"
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

if ($Action -eq "clean") {
    Remove-Item -LiteralPath $baseBuild -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $csBuild -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "MAX-45 clean: PASS"
    exit 0
}

$topDir = Get-NcsTopDir
Assert-NcsBaseline $topDir
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

# Zephyr does not support application/build source paths containing spaces.
# Keep the real checkout untouched and expose the nRF54L15 project through a
# temporary no-space SUBST drive on Windows. This also keeps west's current
# directory and source directory on the same drive during its relpath sanity
# check. Other platforms keep using the original checkout path.
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
    Remove-NoSpaceProjectAlias $sourceAliasDrive
    if ($null -eq $originalZephyrBase) {
        Remove-Item Env:ZEPHYR_BASE -ErrorAction SilentlyContinue
    }
    else {
        $env:ZEPHYR_BASE = $originalZephyrBase
    }
}

Write-Host "MAX-45 ${Action}: PASS" -ForegroundColor Green
