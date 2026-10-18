[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("build", "flash", "monitor")]
    [string]$Action,

    [string]$ProjectPath = ".",
    [string]$BuildDir,
    [string]$Sdkconfig,
    [string]$Port,
    [string]$Target,
    [ValidateSet("default", "debug", "performance", "emulator")]
    [string]$Profile = "default",

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

function Invoke-Checked([string]$FilePath, [object[]]$Arguments, [string]$LogPath = $null) {
    Write-Host ("> {0} {1}" -f $FilePath, ($Arguments -join " "))
    if ([string]::IsNullOrWhiteSpace($LogPath)) {
        & $FilePath @Arguments
    } else {
        $previousErrorActionPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & $FilePath @Arguments 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $LogPath
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
    }
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
$profileResolution = $null
$profileHelperPath = Join-Path $PSScriptRoot "idf_profile.py"
$profileInheritedDefaults = $env:SDKCONFIG_DEFAULTS
$profileCmakeInclude = $null
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = if ($Profile -eq "default") {
        "build"
    } elseif ($Profile -eq "emulator") {
        ".test-build/idf-emulator"
    } else {
        "build-profile-$Profile"
    }
}
$build = if ([IO.Path]::IsPathRooted($BuildDir)) {
    (Resolve-Path -LiteralPath $BuildDir -ErrorAction SilentlyContinue).Path
} else {
    Join-Path $project $BuildDir
}
if ([string]::IsNullOrWhiteSpace($build)) {
    $build = if ([IO.Path]::IsPathRooted($BuildDir)) {
        [IO.Path]::GetFullPath($BuildDir)
    } else {
        Join-Path $project $BuildDir
    }
}
$sdkconfigPath = $null
if (-not [string]::IsNullOrWhiteSpace($Sdkconfig)) {
    $sdkconfigPath = if ([IO.Path]::IsPathRooted($Sdkconfig)) {
        [IO.Path]::GetFullPath($Sdkconfig)
    } else {
        [IO.Path]::GetFullPath((Join-Path $project $Sdkconfig))
    }
}
if ($Profile -ne "default") {
    if ($Action -ne "build") {
        throw "-Profile is supported only with -Action build."
    }
    if (-not (Test-Path -LiteralPath $profileHelperPath -PathType Leaf)) {
        throw "Required ESP-IDF profile resolver is missing: $profileHelperPath"
    }

    $resolveArgs = @(
        $profileHelperPath,
        "resolve",
        "--profile",
        $Profile.ToLowerInvariant(),
        "--project",
        $project
    )
    if (-not [string]::IsNullOrWhiteSpace($profileInheritedDefaults)) {
        $resolveArgs += @("--inherited-defaults", $profileInheritedDefaults)
    }
    $resolutionJson = & $pythonPath @resolveArgs
    if ($LASTEXITCODE -ne 0) {
        throw "ESP-IDF profile resolution failed with exit code $LASTEXITCODE."
    }
    $profileResolution = ($resolutionJson -join [Environment]::NewLine) | ConvertFrom-Json

    $expectedBuild = [IO.Path]::GetFullPath([string]$profileResolution.build_dir)
    if ($PSBoundParameters.ContainsKey("BuildDir") -and
        [IO.Path]::GetFullPath($build) -ne $expectedBuild) {
        throw "-Profile $Profile uses its isolated build directory: $expectedBuild"
    }
    if ($PSBoundParameters.ContainsKey("Sdkconfig") -and
        [IO.Path]::GetFullPath($sdkconfigPath) -ne [IO.Path]::GetFullPath([string]$profileResolution.sdkconfig)) {
        throw "-Profile $Profile uses its isolated sdkconfig: $($profileResolution.sdkconfig)"
    }
    $build = $expectedBuild
    $sdkconfigPath = [string]$profileResolution.sdkconfig
}

$env:IDF_PATH = $idfPath
$env:IDF_TOOLS_PATH = "C:\Espressif"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\python_env\idf5.5_py3.11_env"
$env:ESP_ROM_ELF_DIR = "C:\Espressif\tools\esp-rom-elfs\20241011"
# idf_monitor can receive undecodable/replacement characters from a UART
# stream.  Force the child Python process to write UTF-8 so a redirected
# monitor log cannot crash under a cp1251 PowerShell console.
$env:PYTHONUTF8 = "1"
$env:PYTHONIOENCODING = "utf-8"

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
            $previousSdkconfig = $env:SDKCONFIG
            try {
                if ($profileResolution) {
                    $prepareArgs = @(
                        $profileHelperPath,
                        "prepare-cmake",
                        "--profile",
                        $Profile.ToLowerInvariant(),
                        "--project",
                        $project
                    )
                    if (-not [string]::IsNullOrWhiteSpace($profileInheritedDefaults)) {
                        $prepareArgs += @("--inherited-defaults", $profileInheritedDefaults)
                    }
                    $profileCmakeInclude = & $pythonPath @prepareArgs
                    if ($LASTEXITCODE -ne 0) {
                        throw "ESP-IDF profile CMake preparation failed with exit code $LASTEXITCODE."
                    }
                    $profileCmakeInclude = [string]$profileCmakeInclude
                    $env:SDKCONFIG = $sdkconfigPath
                }

                if (-not (Test-Path -LiteralPath $build -PathType Container)) {
                    New-Item -ItemType Directory -Path $build -Force | Out-Null
                }
                if (-not [string]::IsNullOrWhiteSpace($Target)) {
                    $targetArgs = @($idfPyPath, "-B", $build)
                    if ($profileResolution) {
                        $targetArgs += @("-D", "SDKCONFIG=$sdkconfigPath")
                        $targetArgs += @("-D", "CMAKE_PROJECT_INCLUDE_BEFORE=$profileCmakeInclude")
                    }
                    $targetArgs += @("set-target", $Target)
                    Invoke-Checked $pythonPath $targetArgs
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
                if ($profileResolution) {
                    $reconfigureArgs += @(
                        "-D",
                        "CMAKE_PROJECT_INCLUDE_BEFORE=$profileCmakeInclude"
                    )
                }
                $reconfigureArgs += "reconfigure"
                Invoke-Checked $pythonPath $reconfigureArgs
                Invoke-Checked $ninjaPath @("-C", $build, "-j", $Jobs)

                if ($profileResolution) {
                    Invoke-Checked $pythonPath @($idfPyPath, "-B", $build, "size") `
                        ([string]$profileResolution.size_report)
                    Invoke-Checked $pythonPath @($idfPyPath, "-B", $build, "size-components") `
                        ([string]$profileResolution.size_components_report)

                    $evidenceArgs = @(
                        $profileHelperPath,
                        "record-evidence",
                        "--profile",
                        $Profile.ToLowerInvariant(),
                        "--project",
                        $project
                    )
                    if (-not [string]::IsNullOrWhiteSpace($profileInheritedDefaults)) {
                        $evidenceArgs += @("--inherited-defaults", $profileInheritedDefaults)
                    }
                    $evidencePath = & $pythonPath @evidenceArgs
                    if ($LASTEXITCODE -ne 0) {
                        throw "ESP-IDF build evidence generation failed with exit code $LASTEXITCODE."
                    }
                    Write-Host ("PROFILE_BUILD_EVIDENCE: {0}" -f $evidencePath)
                }
            } finally {
                if ($null -eq $previousSdkconfig) {
                    Remove-Item Env:SDKCONFIG -ErrorAction SilentlyContinue
                } else {
                    $env:SDKCONFIG = $previousSdkconfig
                }
            }
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
