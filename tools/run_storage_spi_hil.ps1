[CmdletBinding()]
param(
    [string]$LogPort = "COM6",
    [string]$ProjectPath = ".",
    [string]$BuildDir = "build",
    [string]$LogPath,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 60000000)]
    [uint64]$MaxMutexWaitUs,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 60000000)]
    [uint64]$MaxTftQuiesceUs,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 60000000)]
    [uint64]$MaxTftResumeUs,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 60000000)]
    [uint64]$MaxSpiEndElapsedUs,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 60000000)]
    [uint64]$MaxRecorderWindowUs,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 60000000)]
    [uint64]$MaxRecorderFwriteUs,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 60000000)]
    [uint64]$MaxRecorderOperationUs,

    [ValidateRange(1, 1000)]
    [int]$MinimumStorageSamples = 1,

    [ValidateRange(1, 1000)]
    [int]$MinimumRecorderSamples = 1,

    [ValidateRange(1, 1000)]
    [int]$MinimumRecorderStopSamples = 1,

    [ValidateRange(1, 8)]
    [int]$MinimumAcquisitionDiagnosticChannels = 8,

    [ValidateRange(5, 600)]
    [int]$CaptureSeconds = 30
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "storage_spi_contract.ps1")


function Read-LogContent {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }

    try {
        return Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
    } catch {
        return ""
    }
}


function Resolve-RepositoryPath {
    param([string]$InputPath)

    $repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
    if ([IO.Path]::IsPathRooted($InputPath)) {
        return (Resolve-Path -LiteralPath $InputPath).Path
    }
    return (Resolve-Path -LiteralPath (Join-Path $repo $InputPath)).Path
}


function Format-Maximum {
    param(
        [string]$Name,
        [AllowNull()]
        [object]$Value,
        [uint64]$Limit
    )

    if ($null -eq $Value) {
        return "$Name=missing limit=${Limit}us"
    }
    return "$Name=$($Value)us limit=${Limit}us"
}


$isReplay = -not [string]::IsNullOrWhiteSpace($LogPath)
$monitor = $null
$logFile = $null
$errorFile = $null
$runnerError = $null

try {
    if ($isReplay) {
        $logFile = (Resolve-Path -LiteralPath $LogPath).Path
    } else {
        $repo = Resolve-RepositoryPath $ProjectPath
        $checkPorts = Join-Path $PSScriptRoot "check_serial_ports.ps1"
        $runIdf = Join-Path $PSScriptRoot "run_idf.ps1"

        if (-not (Test-Path -LiteralPath $checkPorts -PathType Leaf)) {
            throw "Missing serial-port helper: $checkPorts"
        }
        if (-not (Test-Path -LiteralPath $runIdf -PathType Leaf)) {
            throw "Missing ESP-IDF monitor helper: $runIdf"
        }

        & $checkPorts -Ports $LogPort
        if ($LASTEXITCODE -ne 0) {
            throw "Passive UART log port $LogPort is unavailable."
        }

        $powershellCommand = Get-Command powershell.exe -ErrorAction SilentlyContinue
        if ($null -ne $powershellCommand) {
            $powershell = $powershellCommand.Source
        } else {
            $powershell = (Get-Command pwsh -ErrorAction Stop).Source
        }

        $hilDir = Join-Path $repo "build\hil"
        New-Item -ItemType Directory -Path $hilDir -Force | Out-Null
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $logFile = Join-Path $hilDir "storage-spi-$stamp.log"
        $errorFile = Join-Path $hilDir "storage-spi-$stamp.err.log"

        $arguments = @(
            "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", ('"{0}"' -f $runIdf),
            "-Action", "monitor",
            "-ProjectPath", $ProjectPath,
            "-BuildDir", $BuildDir,
            "-Port", $LogPort
        )

        Write-Host "Passive storage/SPI HIL capture on $LogPort. This runner never flashes firmware."
        Write-Host "During the capture, keep AD7606 acquisition healthy, start a recording, generate normal storage activity, then stop the recording normally so STOP_COMPLETE is captured."
        $monitor = Start-Process `
            -FilePath $powershell `
            -ArgumentList $arguments `
            -WorkingDirectory $repo `
            -RedirectStandardOutput $logFile `
            -RedirectStandardError $errorFile `
            -PassThru `
            -WindowStyle Hidden

        Start-Sleep -Seconds $CaptureSeconds
    }
} catch {
    $runnerError = $_.Exception.Message
} finally {
    if ($null -ne $monitor -and -not $monitor.HasExited) {
        Stop-Process -Id $monitor.Id -Force -ErrorAction SilentlyContinue
        $monitor.WaitForExit()
    }
}

$content = if ($null -ne $logFile) { Read-LogContent $logFile } else { "" }
if (-not $isReplay -and $null -ne $errorFile) {
    $content = $content + "`n" + (Read-LogContent $errorFile)
}

$panicPattern = '(?im)(Guru Meditation Error|panic(ed)?|task watchdog|brownout|heap corruption|abort\(\)|assert failed)'
$panicDetected = $content -match $panicPattern

$result = Test-StorageSpiTelemetry `
    -Content $content `
    -MaxMutexWaitUs $MaxMutexWaitUs `
    -MaxTftQuiesceUs $MaxTftQuiesceUs `
    -MaxTftResumeUs $MaxTftResumeUs `
    -MaxSpiEndElapsedUs $MaxSpiEndElapsedUs `
    -MaxRecorderWindowUs $MaxRecorderWindowUs `
    -MaxRecorderFwriteUs $MaxRecorderFwriteUs `
    -MaxRecorderOperationUs $MaxRecorderOperationUs `
    -MinimumStorageSamples $MinimumStorageSamples `
    -MinimumRecorderSamples $MinimumRecorderSamples `
    -MinimumRecorderStopSamples $MinimumRecorderStopSamples `
    -MinimumAcquisitionDiagnosticChannels $MinimumAcquisitionDiagnosticChannels

Write-Host "===== Storage/SPI latency telemetry ====="
Write-Host ("samples storage_begin={0} storage_end={1} recorder_fwrite={2} recorder_stop={3} acquisition_diag_channels={4}" -f `
    $result.StorageBeginCount, $result.StorageEndCount, $result.RecorderCount, `
    $result.RecorderStopCount, $result.AcquisitionDiagnosticChannelCount)
Write-Host (Format-Maximum "mutex_wait_us" $result.MaxMutexWaitUs $MaxMutexWaitUs)
Write-Host (Format-Maximum "tft_quiesce_us" $result.MaxTftQuiesceUs $MaxTftQuiesceUs)
Write-Host (Format-Maximum "tft_resume_us" $result.MaxTftResumeUs $MaxTftResumeUs)
Write-Host (Format-Maximum "spi_end_elapsed_us" $result.MaxSpiEndElapsedUs $MaxSpiEndElapsedUs)
Write-Host (Format-Maximum "max_window_us" $result.MaxRecorderWindowUs $MaxRecorderWindowUs)
Write-Host (Format-Maximum "fwrite_us" $result.MaxRecorderFwriteUs $MaxRecorderFwriteUs)
Write-Host (Format-Maximum "recorder_operation_us" $result.MaxRecorderOperationUs $MaxRecorderOperationUs)
Write-Host ("recorder_dropped_frames={0} expected=0" -f $(if ($null -eq $result.MaxDroppedFrames) { "missing" } else { $result.MaxDroppedFrames }))
Write-Host ("acquisition_failure_markers={0} expected=0" -f $result.AcquisitionFailureCount)
Write-Host ("acquisition_diag_channels={0} required={1}" -f `
    $(if ($result.AcquisitionDiagnosticChannels.Count -eq 0) { "none" } else { $result.AcquisitionDiagnosticChannels -join ',' }), `
    $MinimumAcquisitionDiagnosticChannels)

if ($result.Missing.Count -gt 0) {
    Write-Host "MISSING_TELEMETRY: $($result.Missing -join '; ')"
}
if ($result.Violations.Count -gt 0) {
    foreach ($violation in $result.Violations) {
        Write-Host "THRESHOLD_VIOLATION: $violation"
    }
}
if ($panicDetected) {
    Write-Host "TARGET_FAILURE: panic/watchdog/brownout/assert marker detected"
}
if ($null -ne $runnerError) {
    Write-Host "RUNNER_ERROR: $runnerError"
}

$telemetryStatus = if (
    $result.Violations.Count -gt 0 -or
    $panicDetected
) {
    "FAIL"
} elseif ($result.Missing.Count -gt 0 -or $null -ne $runnerError) {
    "BLOCKED"
} else {
    "PASS"
}

Write-Host "STORAGE_SPI_TELEMETRY_RESULT: $telemetryStatus"

if ($isReplay) {
    Write-Host "REPLAY_LIMITATION: parser/replay validation never proves physical SPI2, SD, TFT DMA, FreeRTOS scheduling, acquisition health, recorder timing, or dropped-frame behavior on the current board."
    $hilStatus = if ($telemetryStatus -eq "FAIL") { "FAIL" } else { "BLOCKED" }
} else {
    $hilStatus = $telemetryStatus
}

Write-Host "STORAGE_SPI_HIL_RESULT: $hilStatus"
if ($null -ne $logFile) {
    Write-Host "LOG: $logFile"
}

if ($hilStatus -eq "PASS") {
    exit 0
}
exit 1
