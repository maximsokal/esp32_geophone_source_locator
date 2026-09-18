[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("build", "flash", "monitor")]
    [string]$Action,

    [string]$ProjectPath = ".",
    [string]$BuildDir = "build",
    [string]$Sdkconfig,
    [string]$Port,
    [string]$Target,

    [ValidateRange(1, 32)]
    [int]$Jobs = 2
)

$ErrorActionPreference = "Stop"

function Resolve-ProjectPath([string]$InputPath) {
    $candidate = if ([IO.Path]::IsPathRooted($InputPath)) {
        $InputPath
    } else {
        Join-Path $PSScriptRoot (Join-Path ".." $InputPath)
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

function Invoke-Checked([string]$FilePath, [object[]]$Arguments) {
    Write-Host ("> {0} {1}" -f $FilePath, ($Arguments -join " "))
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $($LASTEXITCODE): $FilePath"
    }
}


function Sync-RequiredSdkconfig([string]$ConfigPath) {
    if ([string]::IsNullOrWhiteSpace($ConfigPath) -or
        -not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) {
        return
    }

    $content = [IO.File]::ReadAllText($ConfigPath)
    $pattern = '(?m)^CONFIG_FATFS_FS_LOCK=\d+[ \t]*\r?\n?'
    $required = 'CONFIG_FATFS_FS_LOCK=7'
    $matches = [regex]::Matches($content, $pattern)
    if ($matches.Count -eq 1 -and $matches[0].Value.Trim() -eq $required) {
        return
    }

    $withoutStaleValues = [regex]::Replace($content, $pattern, '')
    $updated = $withoutStaleValues.TrimEnd() + [Environment]::NewLine + $required + [Environment]::NewLine
    if ($updated -ne $content) {
        [IO.File]::WriteAllText($ConfigPath, $updated)
        Write-Host ("Migrated local sdkconfig requirement: {0}" -f $required)
    }
}

function Sync-LvglSdkconfigMigration([string]$ConfigPath) {
    if ([string]::IsNullOrWhiteSpace($ConfigPath) -or
        -not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) {
        return
    }

    $content = [IO.File]::ReadAllText($ConfigPath)
    $updated = $content

    # LVGL 9.6 still accepts these LVGL 9.5 sdkconfig entries for
    # compatibility, but emits compiler warnings when they are non-empty.
    # Remove only obsolete non-default values; keep the generated =0/=""
    # entries so a normal subsequent build does not trigger another reconfigure.
    foreach ($pattern in @(
        '(?m)^CONFIG_LV_MEM_SIZE_KILOBYTES=(?!0(?:\r?$)).*(?:\r?\n|$)',
        '(?m)^CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=(?!0(?:\r?$)).*(?:\r?\n|$)',
        '(?m)^CONFIG_LV_ASSERT_HANDLER_INCLUDE="[^\"]+"(?:\r?\n|$)',
        '(?m)^CONFIG_LV_ASSERT_HANDLER_INCLUDE_IS_NON_EMPTY=y(?:\r?\n|$)'
    )) {
        $updated = [regex]::Replace($updated, $pattern, '')
    }

    if ($updated -ne $content) {
        [IO.File]::WriteAllText($ConfigPath, $updated)
        Write-Host "Migrated stale LVGL 9.5 compatibility settings from sdkconfig."
    }
}

$idfPath = "C:\Espressif\frameworks\esp-idf-v5.5.4"
$pythonPath = "C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
$idfPyPath = Join-Path $idfPath "tools\idf.py"
$ninjaPath = "C:\Espressif\tools\ninja\1.12.1\ninja.exe"
$checkPortsPath = Join-Path $PSScriptRoot "check_serial_ports.ps1"

foreach ($required in @($pythonPath, $idfPyPath, $ninjaPath, $checkPortsPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required ESP-IDF tool is missing: $required"
    }
}

$project = Resolve-ProjectPath $ProjectPath
$build = if ([IO.Path]::IsPathRooted($BuildDir)) {
    (Resolve-Path -LiteralPath $BuildDir -ErrorAction SilentlyContinue).Path
} else {
    Join-Path $project $BuildDir
}
if ([string]::IsNullOrWhiteSpace($build)) {
    $build = Join-Path $project $BuildDir
}
$sdkconfigPath = $null
if (-not [string]::IsNullOrWhiteSpace($Sdkconfig)) {
    $sdkconfigPath = if ([IO.Path]::IsPathRooted($Sdkconfig)) {
        [IO.Path]::GetFullPath($Sdkconfig)
    } else {
        [IO.Path]::GetFullPath((Join-Path $project $Sdkconfig))
    }
}

$env:IDF_PATH = $idfPath
$env:IDF_TOOLS_PATH = "C:\Espressif"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\python_env\idf5.5_py3.11_env"
$env:ESP_ROM_ELF_DIR = "C:\Espressif\tools\esp-rom-elfs\20241011"

$toolBins = @(
    "C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin",
    "C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin",
    "C:\Espressif\tools\xtensa-esp-elf-gdb\14.2_20240122\bin",
    "C:\Espressif\tools\riscv32-esp-elf-gdb\14.2_20240122\bin",
    "C:\Espressif\tools\cmake\3.30.2\bin",
    "C:\Espressif\tools\ninja\1.12.1",
    "C:\Espressif\python_env\idf5.5_py3.11_env\Scripts"
)
$env:Path = (($toolBins -join ";") + ";" + [Environment]::GetEnvironmentVariable("Path", "Process"))

Push-Location $project
try {
    Write-Host ("ESP-IDF 5.5.4 | Python: {0}" -f $pythonPath)
    Write-Host ("Project: {0} | Build: {1}" -f $project, $build)

    switch ($Action) {
        "build" {
            if (-not (Test-Path -LiteralPath $build -PathType Container)) {
                New-Item -ItemType Directory -Path $build -Force | Out-Null
            }
            if (-not [string]::IsNullOrWhiteSpace($Target)) {
                Invoke-Checked $pythonPath @($idfPyPath, "-B", $build, "set-target", $Target)
            }
            $effectiveSdkconfig = if ($sdkconfigPath) {
                $sdkconfigPath
            } else {
                Join-Path $project "sdkconfig"
            }
            Sync-RequiredSdkconfig $effectiveSdkconfig
            Sync-LvglSdkconfigMigration $effectiveSdkconfig

            $reconfigureArgs = @($idfPyPath, "-B", $build)
            if ($sdkconfigPath) {
                $reconfigureArgs += @("-D", "SDKCONFIG=$sdkconfigPath")
            }
            $reconfigureArgs += "reconfigure"
            Invoke-Checked $pythonPath $reconfigureArgs
            Invoke-Checked $ninjaPath @("-C", $build, "-j", $Jobs)
        }
        "flash" {
            if ([string]::IsNullOrWhiteSpace($Port)) {
                throw "-Port is required for flash."
            }
            & $checkPortsPath -Ports $Port
            if (-not $?) {
                throw "Serial port check failed."
            }
            Invoke-Checked $pythonPath @($idfPyPath, "-B", $build, "-p", $Port, "flash")
        }
        "monitor" {
            if ([string]::IsNullOrWhiteSpace($Port)) {
                throw "-Port is required for monitor."
            }
            & $checkPortsPath -Ports $Port
            if (-not $?) {
                throw "Serial port check failed."
            }
            Invoke-Checked $pythonPath @($idfPyPath, "-B", $build, "-p", $Port, "monitor")
        }
    }
} finally {
    Pop-Location
}
