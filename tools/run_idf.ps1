[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("build", "flash", "monitor")]
    [string]$Action,

    [string]$ProjectPath = ".",
    [string]$BuildDir = "build",
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
            Invoke-Checked $pythonPath @($idfPyPath, "-B", $build, "reconfigure")
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
