[CmdletBinding()]
param(
    [string]$LogPort = "COM6",
    [string]$ProjectPath = ".",
    [string]$BuildDir = "build",
    [string]$LogPath,
    [string]$BaseUrl = "",
    [string]$HealthEvidencePath = "",

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
    [int]$CaptureSeconds = 30,

    [ValidateRange(1, 60)]
    [int]$HealthSettleSeconds = 15
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


function Get-OptionalProperty {
    param(
        [AllowNull()][object]$Object,
        [string]$Name
    )
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}


function Get-Ad7606TransportHealth {
    param([AllowNull()][object]$Status)

    $acquisition = Get-OptionalProperty $Status "acquisition"
    if ($null -eq $acquisition) {
        return [pscustomobject]@{
            Status = "BLOCKED"; Reason = "missing acquisition health object"; SignalQuality = "UNKNOWN"; Window = $null
        }
    }

    $serial = Get-OptionalProperty $acquisition "serial"
    $signal = Get-OptionalProperty $acquisition "signal"
    $countersAvailable = Get-OptionalProperty $acquisition "error_counters_available"
    $consecutiveFailures = Get-OptionalProperty $acquisition "consecutive_failures"
    $quality = [string](Get-OptionalProperty $serial "quality")
    $streamState = [string](Get-OptionalProperty $serial "stream_state")
    $suspicious = Get-OptionalProperty $serial "suspicious"
    $streamStuck = Get-OptionalProperty $serial "stream_stuck"
    $windowValue = Get-OptionalProperty $serial "quality_window_frames"
    $signalQuality = [string](Get-OptionalProperty $signal "quality")

    if ([string]::IsNullOrWhiteSpace($quality)) { $quality = "UNKNOWN" }
    if ([string]::IsNullOrWhiteSpace($streamState)) { $streamState = "UNKNOWN" }
    if ([string]::IsNullOrWhiteSpace($signalQuality)) { $signalQuality = "UNKNOWN" }
    $quality = $quality.ToUpperInvariant()
    $streamState = $streamState.ToUpperInvariant()
    $signalQuality = $signalQuality.ToUpperInvariant()

    if ($countersAvailable -ne $true) {
        return [pscustomobject]@{
            Status = "BLOCKED"; Reason = "error_counters_available is not true"; SignalQuality = $signalQuality; Window = $windowValue
        }
    }

    $window = 0
    if ($null -eq $windowValue -or -not [int]::TryParse([string]$windowValue, ([ref]$window))) {
        return [pscustomobject]@{
            Status = "BLOCKED"; Reason = "serial quality_window_frames is missing"; SignalQuality = $signalQuality; Window = $null
        }
    }
    if ($window -lt 500) {
        return [pscustomobject]@{
            Status = "BLOCKED"; Reason = "serial quality window immature ($window/500)"; SignalQuality = $signalQuality; Window = $window
        }
    }

    $reasons = New-Object System.Collections.Generic.List[string]
    if ($quality -eq "DEGRADED" -or $quality -eq "FAULT") {
        $reasons.Add("transport quality=$quality")
    }
    if ($streamState -eq "SUSPICIOUS" -or $streamState -eq "STUCK") {
        $reasons.Add("stream state=$streamState")
    }
    if ($suspicious -eq $true) { $reasons.Add("serial suspicious=true") }
    if ($streamStuck -eq $true) { $reasons.Add("serial stream_stuck=true") }

    $consecutive = 0
    if ($null -ne $consecutiveFailures -and [int]::TryParse([string]$consecutiveFailures, ([ref]$consecutive))) {
        if ($consecutive -ne 0) { $reasons.Add("consecutive_failures=$consecutive expected=0") }
    } else {
        return [pscustomobject]@{
            Status = "BLOCKED"; Reason = "consecutive_failures is unavailable"; SignalQuality = $signalQuality; Window = $window
        }
    }

    if ($reasons.Count -gt 0) {
        return [pscustomobject]@{
            Status = "FAIL"; Reason = ($reasons -join ", "); SignalQuality = $signalQuality; Window = $window
        }
    }
    if ($quality -ne "HEALTHY") {
        return [pscustomobject]@{
            Status = "BLOCKED"; Reason = "transport quality=$quality"; SignalQuality = $signalQuality; Window = $window
        }
    }
    if ($streamState -ne "NORMAL" -and $streamState -ne "QUIET") {
        return [pscustomobject]@{
            Status = "BLOCKED"; Reason = "stream state=$streamState"; SignalQuality = $signalQuality; Window = $window
        }
    }
    if ($suspicious -ne $false -or $streamStuck -ne $false) {
        return [pscustomobject]@{
            Status = "BLOCKED"; Reason = "serial suspicious/stuck flags are unavailable"; SignalQuality = $signalQuality; Window = $window
        }
    }

    return [pscustomobject]@{
        Status = "PASS"; Reason = "healthy transport"; SignalQuality = $signalQuality; Window = $window
    }
}


function Get-HealthEvidenceStatus {
    param([AllowNull()][object]$Evidence)

    if ($null -eq $Evidence) {
        return [pscustomobject]@{ Status = "BLOCKED"; Reason = "missing AD7606 transport health evidence"; Count = 0 }
    }
    $samples = @(Get-OptionalProperty $Evidence "samples")
    if ($samples.Count -eq 0 -or $null -eq $samples[0]) {
        return [pscustomobject]@{ Status = "BLOCKED"; Reason = "missing AD7606 transport health evidence"; Count = 0 }
    }

    $blockedReasons = New-Object System.Collections.Generic.List[string]
    foreach ($sample in $samples) {
        $phase = [string](Get-OptionalProperty $sample "phase")
        if ([string]::IsNullOrWhiteSpace($phase)) { $phase = "sample" }
        $rawStatus = Get-OptionalProperty $sample "status"
        if ($null -eq $rawStatus) { $rawStatus = $sample }
        $classification = Get-Ad7606TransportHealth $rawStatus
        if ($classification.Status -eq "FAIL") {
            return [pscustomobject]@{ Status = "FAIL"; Reason = "$phase`: $($classification.Reason)"; Count = $samples.Count }
        }
        if ($classification.Status -eq "BLOCKED") {
            $blockedReasons.Add("$phase`: $($classification.Reason)")
        }
    }
    if ($blockedReasons.Count -gt 0) {
        return [pscustomobject]@{ Status = "BLOCKED"; Reason = ($blockedReasons -join "; "); Count = $samples.Count }
    }
    return [pscustomobject]@{ Status = "PASS"; Reason = "all saved health samples are healthy"; Count = $samples.Count }
}


function Get-UartTransportFaults {
    param([string]$Content)

    $faults = New-Object System.Collections.Generic.List[string]
    $pattern = '(?im)AD7606_SERIAL_DIAG:.*?transport_quality=(?<quality>[A-Z_]+).*?signal_quality=(?<signal>[A-Z_]+).*?stream_state=(?<stream>[A-Z_]+).*?stream_stuck=(?<stuck>yes|no).*?window=(?<window>\d+).*?serial_suspicious=(?<suspicious>yes|no)'
    foreach ($match in [regex]::Matches($Content, $pattern)) {
        $window = [int]$match.Groups['window'].Value
        if ($window -lt 500) { continue }
        $quality = $match.Groups['quality'].Value.ToUpperInvariant()
        $stream = $match.Groups['stream'].Value.ToUpperInvariant()
        $stuck = $match.Groups['stuck'].Value -ieq 'yes'
        $suspicious = $match.Groups['suspicious'].Value -ieq 'yes'
        $reasons = New-Object System.Collections.Generic.List[string]
        if ($quality -eq 'DEGRADED' -or $quality -eq 'FAULT') { $reasons.Add("transport quality=$quality") }
        if ($stream -eq 'SUSPICIOUS' -or $stream -eq 'STUCK') { $reasons.Add("stream state=$stream") }
        if ($suspicious) { $reasons.Add('serial suspicious=true') }
        if ($stuck) { $reasons.Add('serial stream_stuck=true') }
        if ($reasons.Count -gt 0) {
            $faults.Add("window=$window $($reasons -join ', ')")
        }
    }
    return @($faults)
}


function Save-HealthEvidence {
    param(
        [object[]]$Samples,
        [string]$Path
    )
    [pscustomobject]@{ samples = @($Samples) } |
        ConvertTo-Json -Depth 12 |
        Set-Content -LiteralPath $Path -Encoding UTF8
}


$isReplay = -not [string]::IsNullOrWhiteSpace($LogPath)
$monitor = $null
$logFile = $null
$errorFile = $null
$runnerError = $null
$healthFile = $null
$healthEvidence = $null
$healthStatus = "BLOCKED"
$healthReason = "health evidence not evaluated"
$healthSamples = New-Object System.Collections.Generic.List[object]
$repo = $null

try {
    if ($isReplay) {
        $logFile = (Resolve-Path -LiteralPath $LogPath).Path
        if (-not [string]::IsNullOrWhiteSpace($HealthEvidencePath)) {
            $resolvedHealth = (Resolve-Path -LiteralPath $HealthEvidencePath).Path
            $healthEvidence = Get-Content -LiteralPath $resolvedHealth -Raw | ConvertFrom-Json
            $healthFile = $resolvedHealth
        }
    } else {
        if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
            throw "Live storage/SPI HIL requires -BaseUrl for /api/status transport-health preflight."
        }
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

        $base = $BaseUrl.TrimEnd('/')
        $deadline = (Get-Date).AddSeconds($HealthSettleSeconds)
        $preflightStatus = $null
        $preflightHealth = $null
        do {
            $preflightStatus = Invoke-RestMethod -Uri ("{0}/api/status" -f $base) -Method GET -TimeoutSec 20 -ErrorAction Stop
            $preflightHealth = Get-Ad7606TransportHealth $preflightStatus
            if ($preflightHealth.Status -eq "PASS" -or $preflightHealth.Status -eq "FAIL") { break }
            Start-Sleep -Milliseconds 500
        } while ((Get-Date) -lt $deadline)

        $healthSamples.Add([pscustomobject]@{ phase = "preflight"; status = $preflightStatus }) | Out-Null
        if ($preflightHealth.Status -eq "FAIL") {
            $healthStatus = "FAIL"
            $healthReason = "preflight: $($preflightHealth.Reason)"
            throw "AD7606 transport preflight failed: $($preflightHealth.Reason)"
        }
        if ($preflightHealth.Status -ne "PASS") {
            $healthStatus = "BLOCKED"
            $healthReason = "preflight: $($preflightHealth.Reason)"
            throw "AD7606 transport preflight did not become mature and healthy within ${HealthSettleSeconds}s: $($preflightHealth.Reason)"
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
        $healthFile = Join-Path $hilDir "storage-spi-$stamp.health.json"

        $arguments = @(
            "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", ('"{0}"' -f $runIdf),
            "-Action", "monitor",
            "-ProjectPath", $ProjectPath,
            "-BuildDir", $BuildDir,
            "-Port", $LogPort
        )

        Write-Host "Passive storage/SPI HIL capture on $LogPort. This runner never flashes firmware."
        Write-Host "AD7606 transport preflight: PASS window=$($preflightHealth.Window) signal_quality=$($preflightHealth.SignalQuality)."
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

        $postStatus = Invoke-RestMethod -Uri ("{0}/api/status" -f $base) -Method GET -TimeoutSec 20 -ErrorAction Stop
        $healthSamples.Add([pscustomobject]@{ phase = "post"; status = $postStatus }) | Out-Null
        Save-HealthEvidence @($healthSamples) $healthFile
        $healthEvidence = [pscustomobject]@{ samples = @($healthSamples) }
    }
} catch {
    $runnerError = $_.Exception.Message
} finally {
    if ($null -ne $monitor -and -not $monitor.HasExited) {
        Stop-Process -Id $monitor.Id -Force -ErrorAction SilentlyContinue
        $monitor.WaitForExit()
    }
    if (-not $isReplay -and $healthSamples.Count -gt 0) {
        try {
            if ($null -eq $healthFile -and $null -ne $repo) {
                $hilDir = Join-Path $repo "build\hil"
                New-Item -ItemType Directory -Path $hilDir -Force | Out-Null
                $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
                $healthFile = Join-Path $hilDir "storage-spi-$stamp.health.json"
            }
            if ($null -ne $healthFile) {
                Save-HealthEvidence @($healthSamples) $healthFile
            }
        } catch { }
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

$healthGate = Get-HealthEvidenceStatus $healthEvidence
if ($healthStatus -ne "FAIL") {
    $healthStatus = $healthGate.Status
    $healthReason = $healthGate.Reason
}
$uartTransportFaults = @(Get-UartTransportFaults $content)
if ($uartTransportFaults.Count -gt 0) {
    $healthStatus = "FAIL"
    $healthReason = "mature in-capture UART transport fault: $($uartTransportFaults -join '; ')"
}

$droppedFramesText = "missing"
if ($null -ne $result.MaxDroppedFrames) {
    $droppedFramesText = [string]$result.MaxDroppedFrames
}

$diagnosticChannelsText = "none"
if ($result.AcquisitionDiagnosticChannels.Count -gt 0) {
    $diagnosticChannelsText = $result.AcquisitionDiagnosticChannels -join ','
}

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
Write-Host ("recorder_dropped_frames={0} expected=0" -f $droppedFramesText)
Write-Host ("acquisition_failure_markers={0} expected=0" -f $result.AcquisitionFailureCount)
Write-Host ("acquisition_diag_channels={0} required={1}" -f `
    $diagnosticChannelsText, $MinimumAcquisitionDiagnosticChannels)
Write-Host "AD7606_TRANSPORT_HEALTH: $healthStatus ($healthReason)"

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
if ($uartTransportFaults.Count -gt 0) {
    foreach ($fault in $uartTransportFaults) {
        Write-Host "AD7606_TRANSPORT_FAULT: $fault"
    }
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

if ($telemetryStatus -eq "FAIL" -or $healthStatus -eq "FAIL") {
    $combinedStatus = "FAIL"
} elseif ($telemetryStatus -eq "BLOCKED" -or $healthStatus -eq "BLOCKED") {
    $combinedStatus = "BLOCKED"
} else {
    $combinedStatus = "PASS"
}

if ($isReplay) {
    Write-Host "REPLAY_LIMITATION: parser/replay validation never proves physical SPI2, SD, TFT DMA, FreeRTOS scheduling, acquisition health, recorder timing, or dropped-frame behavior on the current board."
    $hilStatus = if ($combinedStatus -eq "FAIL") { "FAIL" } else { "BLOCKED" }
} else {
    $hilStatus = $combinedStatus
}

Write-Host "STORAGE_SPI_HIL_RESULT: $hilStatus"
if ($null -ne $logFile) {
    Write-Host "LOG: $logFile"
}
if ($null -ne $healthFile) {
    Write-Host "HEALTH_EVIDENCE: $healthFile"
}

if ($hilStatus -eq "PASS") {
    exit 0
}
exit 1
