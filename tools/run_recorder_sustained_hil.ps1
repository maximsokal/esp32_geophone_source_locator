[CmdletBinding()]
param(
    [string]$LogPort = "COM6",
    [string]$ProjectPath = ".",
    [string]$BuildDir = "build",

    [Parameter(Mandatory = $true)]
    [string]$BaseUrl,

    [Parameter(Mandatory = $true)]
    [string]$ObjectId,

    [ValidateRange(1, 40000)]
    [int]$ExpectedSampleRateHz = 500,

    [ValidateRange(1, 256)]
    [int]$FrameSizeBytes = 24,

    [ValidateRange(1.0, 100.0)]
    [double]$SafetyFactor = 4.0,

    [ValidateRange(16, 1200)]
    [int]$ReferencePoints = 128,

    [ValidateRange(16, 1200)]
    [int]$SavedReferenceFrames = 1200,

    [ValidateRange(16, 1200)]
    [int]$MinimumOverlapFrames = 64,

    [ValidateRange(0, 100000)]
    [int]$EvictionMarginFrames = 500,

    [ValidateRange(100, 5000)]
    [int]$ReferenceSettleMs = 500,

    [ValidateRange(1, 1000)]
    [int]$MinimumWriteSamples = 30,

    [ValidateRange(30, 900)]
    [int]$MaxRecordingSeconds = 180,

    [ValidateRange(100, 5000)]
    [int]$PollIntervalMs = 1000,

    [ValidateRange(1, 60)]
    [int]$HealthSettleSeconds = 15
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest


function Read-LogContent {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }
    try {
        return Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
    } catch {
        return ""
    }
}


function Resolve-RepoPath {
    param([string]$InputPath)

    $repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
    if ([IO.Path]::IsPathRooted($InputPath)) {
        return (Resolve-Path -LiteralPath $InputPath).Path
    }
    return (Resolve-Path -LiteralPath (Join-Path $repo $InputPath)).Path
}


function Invoke-JsonRequest {
    param(
        [string]$Uri,
        [ValidateSet("GET", "POST")]
        [string]$Method = "GET",
        [string]$Body = ""
    )

    $params = @{
        Uri = $Uri
        Method = $Method
        TimeoutSec = 20
        ErrorAction = "Stop"
    }
    if (-not [string]::IsNullOrWhiteSpace($Body)) {
        $params.Body = $Body
        $params.ContentType = "application/json"
    }
    return Invoke-RestMethod @params
}


function Save-JsonEvidence {
    param(
        [object]$Value,
        [string]$Path
    )

    $Value |
        ConvertTo-Json -Depth 12 -Compress |
        Set-Content -LiteralPath $Path -Encoding UTF8
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


function Get-ReportedSampleRateHz {
    param([AllowNull()][object]$Status)

    $acquisition = Get-OptionalProperty $Status "acquisition"
    $rate = 0
    if ($null -ne $acquisition -and
        [int]::TryParse([string](Get-OptionalProperty $acquisition "sample_rate_hz"), ([ref]$rate)) -and
        $rate -gt 0) {
        return $rate
    }
    return 500
}


function Get-Ad7606TransportHealth {
    param([AllowNull()][object]$Status)

    $acquisition = Get-OptionalProperty $Status "acquisition"
    if ($null -eq $acquisition) {
        return [pscustomobject]@{ Status = "BLOCKED"; Reason = "missing acquisition health object"; SignalQuality = "UNKNOWN"; Window = $null }
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
    $requiredWindowFrames = Get-ReportedSampleRateHz $Status
    $signalQuality = [string](Get-OptionalProperty $signal "quality")

    if ([string]::IsNullOrWhiteSpace($quality)) { $quality = "UNKNOWN" }
    if ([string]::IsNullOrWhiteSpace($streamState)) { $streamState = "UNKNOWN" }
    if ([string]::IsNullOrWhiteSpace($signalQuality)) { $signalQuality = "UNKNOWN" }
    $quality = $quality.ToUpperInvariant()
    $streamState = $streamState.ToUpperInvariant()
    $signalQuality = $signalQuality.ToUpperInvariant()

    if ($countersAvailable -ne $true) {
        return [pscustomobject]@{ Status = "BLOCKED"; Reason = "error_counters_available is not true"; SignalQuality = $signalQuality; Window = $windowValue }
    }
    $window = 0
    if ($null -eq $windowValue -or -not [int]::TryParse([string]$windowValue, ([ref]$window))) {
        return [pscustomobject]@{ Status = "BLOCKED"; Reason = "serial quality_window_frames is missing"; SignalQuality = $signalQuality; Window = $null }
    }
    if ($window -lt $requiredWindowFrames) {
        return [pscustomobject]@{ Status = "BLOCKED"; Reason = "serial quality window immature ($window/$requiredWindowFrames frames, one second at $(Get-ReportedSampleRateHz $Status)Hz)"; SignalQuality = $signalQuality; Window = $window }
    }

    $reasons = New-Object System.Collections.Generic.List[string]
    if ($quality -eq "DEGRADED" -or $quality -eq "FAULT") { $reasons.Add("transport quality=$quality") }
    if ($streamState -eq "SUSPICIOUS" -or $streamState -eq "STUCK") { $reasons.Add("stream state=$streamState") }
    if ($suspicious -eq $true) { $reasons.Add("serial suspicious=true") }
    if ($streamStuck -eq $true) { $reasons.Add("serial stream_stuck=true") }

    $consecutive = 0
    if ($null -ne $consecutiveFailures -and [int]::TryParse([string]$consecutiveFailures, ([ref]$consecutive))) {
        if ($consecutive -ne 0) { $reasons.Add("consecutive_failures=$consecutive expected=0") }
    } else {
        return [pscustomobject]@{ Status = "BLOCKED"; Reason = "consecutive_failures is unavailable"; SignalQuality = $signalQuality; Window = $window }
    }

    if ($reasons.Count -gt 0) {
        return [pscustomobject]@{ Status = "FAIL"; Reason = ($reasons -join ", "); SignalQuality = $signalQuality; Window = $window }
    }
    if ($quality -ne "HEALTHY") {
        return [pscustomobject]@{ Status = "BLOCKED"; Reason = "transport quality=$quality"; SignalQuality = $signalQuality; Window = $window }
    }
    if ($streamState -ne "NORMAL" -and $streamState -ne "QUIET") {
        return [pscustomobject]@{ Status = "BLOCKED"; Reason = "stream state=$streamState"; SignalQuality = $signalQuality; Window = $window }
    }
    if ($suspicious -ne $false -or $streamStuck -ne $false) {
        return [pscustomobject]@{ Status = "BLOCKED"; Reason = "serial suspicious/stuck flags are unavailable"; SignalQuality = $signalQuality; Window = $window }
    }
    return [pscustomobject]@{ Status = "PASS"; Reason = "healthy transport"; SignalQuality = $signalQuality; Window = $window }
}


function Add-HealthSample {
    param(
        [System.Collections.Generic.List[object]]$Samples,
        [string]$Phase,
        [object]$Status
    )
    $Samples.Add([pscustomobject]@{ phase = $Phase; status = $Status }) | Out-Null
    return Get-Ad7606TransportHealth $Status
}


function Stop-RecordingBestEffort {
    param([string]$RootUrl)

    try {
        $body = @{ action = "recording_stop" } | ConvertTo-Json -Compress
        [void](Invoke-JsonRequest ("{0}/api/control" -f $RootUrl) "POST" $body)
    } catch {
        # Preserve the primary runner failure.
    }
}


$repo = Resolve-RepoPath $ProjectPath
$base = $BaseUrl.TrimEnd("/")
$monitor = $null
$logFile = $null
$errorFile = $null
$liveJson = $null
$savedJson = $null
$healthJson = $null
$recordingStarted = $false
$sessionId = $null
$runnerError = $null
$analysisExit = $null
$transportFaultObserved = $false
$healthSamples = New-Object System.Collections.Generic.List[object]

try {
    $checkPorts = Join-Path $PSScriptRoot "check_serial_ports.ps1"
    $runIdf = Join-Path $PSScriptRoot "run_idf.ps1"
    $analyzer = Join-Path $PSScriptRoot "analyze_recorder_sustained.py"

    foreach ($required in @($checkPorts, $runIdf, $analyzer)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Missing required helper: $required"
        }
    }

    & $checkPorts -Ports $LogPort
    if ($LASTEXITCODE -ne 0) {
        throw "Passive UART log port $LogPort is unavailable."
    }

    $preflightDeadline = (Get-Date).AddSeconds($HealthSettleSeconds)
    $initialStatus = $null
    $preflightHealth = $null
    do {
        $initialStatus = Invoke-JsonRequest ("{0}/api/status" -f $base)
        $preflightHealth = Get-Ad7606TransportHealth $initialStatus
        if ($preflightHealth.Status -eq "PASS" -or $preflightHealth.Status -eq "FAIL") { break }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $preflightDeadline)
    $preflightHealth = Add-HealthSample $healthSamples "preflight" $initialStatus

    if ($preflightHealth.Status -eq "FAIL") {
        $transportFaultObserved = $true
        throw "AD7606 transport preflight failed: $($preflightHealth.Reason)"
    }
    if ($preflightHealth.Status -ne "PASS") {
        throw "AD7606 transport health did not become mature within ${HealthSettleSeconds}s: $($preflightHealth.Reason)"
    }
    if ($null -eq $initialStatus.acquisition -or $initialStatus.acquisition.running -ne $true) {
        throw "Acquisition is not running; sustained recorder HIL requires the live AD7606 pipeline."
    }
    if ([int]$initialStatus.acquisition.sample_rate_hz -ne $ExpectedSampleRateHz) {
        throw "Acquisition rate is $($initialStatus.acquisition.sample_rate_hz)Hz, expected ${ExpectedSampleRateHz}Hz."
    }
    if ($null -ne $initialStatus.recording -and $initialStatus.recording.running -eq $true) {
        throw "A recording is already active. Stop it before starting the isolated sustained HIL run."
    }

    $powershellCommand = Get-Command powershell.exe -ErrorAction SilentlyContinue
    $powershell = if ($null -ne $powershellCommand) {
        $powershellCommand.Source
    } else {
        (Get-Command pwsh -ErrorAction Stop).Source
    }
    $python = (Get-Command python -ErrorAction Stop).Source

    $hilDir = Join-Path $repo "build\hil"
    New-Item -ItemType Directory -Path $hilDir -Force | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $logFile = Join-Path $hilDir "recorder-sustained-$stamp.log"
    $errorFile = Join-Path $hilDir "recorder-sustained-$stamp.err.log"
    $liveJson = Join-Path $hilDir "recorder-sustained-$stamp.live.json"
    $savedJson = Join-Path $hilDir "recorder-sustained-$stamp.saved.json"
    $healthJson = Join-Path $hilDir "recorder-sustained-$stamp.health.json"

    Write-Host "AD7606 transport preflight: PASS window=$($preflightHealth.Window) signal_quality=$($preflightHealth.SignalQuality)."

    $monitorArgs = @(
        "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", ('"{0}"' -f $runIdf),
        "-Action", "monitor",
        "-ProjectPath", $ProjectPath,
        "-BuildDir", $BuildDir,
        "-Port", $LogPort
    )

    Write-Host "Starting passive sustained-recorder HIL capture on $LogPort. This runner never flashes firmware."
    $monitor = Start-Process `
        -FilePath $powershell `
        -ArgumentList $monitorArgs `
        -WorkingDirectory $repo `
        -RedirectStandardOutput $logFile `
        -RedirectStandardError $errorFile `
        -PassThru `
        -WindowStyle Hidden

    Start-Sleep -Seconds 2
    if ($monitor.HasExited) {
        throw "COM6 monitor exited before the HIL run. See $errorFile."
    }

    $startBody = @{
        action = "recording_start"
        object_id = $ObjectId
    } | ConvertTo-Json -Compress
    $startResponse = Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $startBody
    if ($startResponse.ok -ne $true) {
        throw "recording_start was rejected."
    }
    $recordingStarted = $true

    $startDeadline = (Get-Date).AddSeconds(20)
    $status = $null
    do {
        Start-Sleep -Milliseconds $PollIntervalMs
        $status = Invoke-JsonRequest ("{0}/api/status" -f $base)
        $health = Add-HealthSample $healthSamples "during-start" $status
        if ($health.Status -eq "FAIL") {
            $transportFaultObserved = $true
            throw "AD7606 transport fault while waiting for recorder start: $($health.Reason)"
        }
        if ($health.Status -ne "PASS") {
            throw "AD7606 transport health became unavailable while waiting for recorder start: $($health.Reason)"
        }
        if ($status.recording.running -eq $true) {
            $sessionId = [string]$status.recording.session_id
            break
        }
    } while ((Get-Date) -lt $startDeadline)

    if ([string]::IsNullOrWhiteSpace($sessionId)) {
        throw "recording_start was accepted but no active session_id appeared within 20 seconds."
    }

    Start-Sleep -Milliseconds $ReferenceSettleMs
    $liveReference = Invoke-JsonRequest (
        "{0}/api/seismic?points={1}" -f $base, $ReferencePoints
    )
    if (
        $liveReference.source -ne "AD7606" -or
        [int]$liveReference.sample_rate_hz -ne $ExpectedSampleRateHz -or
        [int]$liveReference.count -lt $MinimumOverlapFrames
    ) {
        throw "Live integrity reference is incomplete or not the expected AD7606 stream."
    }
    Save-JsonEvidence $liveReference $liveJson

    $capacityDeadline = (Get-Date).AddSeconds(10)
    $cacheCapacityFrames = 0
    $sessionPattern = [regex]::Escape($sessionId)
    do {
        $content = Read-LogContent $logFile
        $match = [regex]::Match(
            $content,
            "(?im)Recording\s+[^/\s]+/${sessionPattern}\s+at\s+(\d+)\s+Hz.*?PSRAM\s+cache=(\d+)\s+frames"
        )
        if ($match.Success) {
            $loggedRate = [int]$match.Groups[1].Value
            $cacheCapacityFrames = [int]$match.Groups[2].Value
            if ($loggedRate -ne $ExpectedSampleRateHz) {
                throw "Recorder start marker rate=${loggedRate}Hz, expected ${ExpectedSampleRateHz}Hz."
            }
            break
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $capacityDeadline)

    if ($cacheCapacityFrames -le 0) {
        throw "Recorder start marker did not expose PSRAM cache capacity on COM6."
    }

    [uint64]$requiredPersistedFrames =
        [uint64]$cacheCapacityFrames +
        [uint64]$SavedReferenceFrames +
        [uint64]$EvictionMarginFrames
    $minimumSeconds = [math]::Ceiling(
        [double]$requiredPersistedFrames / [double]$ExpectedSampleRateHz
    )
    Write-Host (
        "Sustained gate: session={0} cache={1} frames; require persisted>{2} frames (~{3}s) so frames 0..{4} are evicted from PSRAM." -f
        $sessionId, $cacheCapacityFrames, $requiredPersistedFrames, $minimumSeconds, ($SavedReferenceFrames - 1)
    )

    $recordDeadline = (Get-Date).AddSeconds($MaxRecordingSeconds)
    do {
        Start-Sleep -Milliseconds $PollIntervalMs
        $status = Invoke-JsonRequest ("{0}/api/status" -f $base)
        $health = Add-HealthSample $healthSamples "during-recording" $status
        if ($health.Status -eq "FAIL") {
            $transportFaultObserved = $true
            throw "AD7606 transport fault during recording: $($health.Reason)"
        }
        if ($health.Status -ne "PASS") {
            throw "AD7606 transport health became unavailable during recording: $($health.Reason)"
        }
        if ($status.recording.running -ne $true) {
            throw "Recording stopped unexpectedly before the eviction/throughput gate completed."
        }
        if ([uint64]$status.recording.dropped_frames -ne 0) {
            throw "Recorder reported dropped_frames=$($status.recording.dropped_frames) during sustained run."
        }
        if ([uint64]$status.recording.samples_written -gt $requiredPersistedFrames) {
            break
        }
    } while ((Get-Date) -lt $recordDeadline)

    if ([uint64]$status.recording.samples_written -le $requiredPersistedFrames) {
        throw (
            "Timed out after {0}s before cache eviction proof: samples_written={1}, required>{2}." -f
            $MaxRecordingSeconds, $status.recording.samples_written, $requiredPersistedFrames
        )
    }

    $stopBody = @{ action = "recording_stop" } | ConvertTo-Json -Compress
    $stopResponse = Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $stopBody
    if ($stopResponse.ok -ne $true) {
        throw "recording_stop was rejected."
    }

    $stopDeadline = (Get-Date).AddSeconds(45)
    do {
        Start-Sleep -Milliseconds $PollIntervalMs
        $status = Invoke-JsonRequest ("{0}/api/status" -f $base)
        $phase = if ($status.recording.running -eq $true) { "during-stop" } else { "post" }
        $health = Add-HealthSample $healthSamples $phase $status
        if ($health.Status -eq "FAIL") {
            $transportFaultObserved = $true
            throw "AD7606 transport fault while stopping recording: $($health.Reason)"
        }
        if ($health.Status -ne "PASS") {
            throw "AD7606 transport health became unavailable while stopping recording: $($health.Reason)"
        }
        if ($status.recording.running -ne $true) {
            break
        }
    } while ((Get-Date) -lt $stopDeadline)

    if ($status.recording.running -eq $true) {
        throw "recording_stop did not complete within 45 seconds."
    }
    $recordingStarted = $false
    if ([string]$status.recording.session_id -ne $sessionId) {
        throw "Session changed from $sessionId to $($status.recording.session_id) after STOP."
    }
    if ([uint64]$status.recording.dropped_frames -ne 0) {
        throw "Completed session has dropped_frames=$($status.recording.dropped_frames)."
    }

    Save-JsonEvidence ([pscustomobject]@{ samples = @($healthSamples) }) $healthJson

    $encodedObject = [uri]::EscapeDataString($ObjectId)
    $encodedSession = [uri]::EscapeDataString($sessionId)
    $savedUri = (
        "{0}/api/seismic/session?object_id={1}&session_id={2}&start_frame=0&frame_count={3}&points={3}" -f
        $base, $encodedObject, $encodedSession, $SavedReferenceFrames
    )
    $savedReference = Invoke-JsonRequest $savedUri
    Save-JsonEvidence $savedReference $savedJson

    # Give the passive monitor one complete logging interval to flush the
    # WAVEFORM_READ_PATH source=raw evidence emitted by the SD reader.
    Start-Sleep -Seconds 1

    if ($null -ne $monitor -and -not $monitor.HasExited) {
        Stop-Process -Id $monitor.Id -Force -ErrorAction SilentlyContinue
        $monitor.WaitForExit()
    }
    $monitor = $null

    $analyzerArgs = @(
        $analyzer,
        $logFile,
        $liveJson,
        $savedJson,
        "--health-evidence", $healthJson,
        "--sample-rate-hz", [string]$ExpectedSampleRateHz,
        "--frame-size-bytes", [string]$FrameSizeBytes,
        "--safety-factor", [string]$SafetyFactor,
        "--minimum-write-samples", [string]$MinimumWriteSamples,
        "--minimum-overlap-frames", [string]$MinimumOverlapFrames
    )
    & $python @analyzerArgs
    $analysisExit = $LASTEXITCODE
} catch {
    $runnerError = $_.Exception.Message
} finally {
    if ($recordingStarted) {
        Stop-RecordingBestEffort $base
    }
    if ($null -ne $monitor -and -not $monitor.HasExited) {
        Stop-Process -Id $monitor.Id -Force -ErrorAction SilentlyContinue
        $monitor.WaitForExit()
    }
    if ($healthSamples.Count -gt 0) {
        try {
            if ($null -eq $healthJson) {
                $hilDir = Join-Path $repo "build\hil"
                New-Item -ItemType Directory -Path $hilDir -Force | Out-Null
                $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
                $healthJson = Join-Path $hilDir "recorder-sustained-$stamp.health.json"
            }
            Save-JsonEvidence ([pscustomobject]@{ samples = @($healthSamples) }) $healthJson
        } catch {
            # Preserve the primary runner failure.
        }
    }
}

if ($null -ne $runnerError) {
    Write-Host "RUNNER_ERROR: $runnerError"
    $failureStatus = if ($transportFaultObserved) { "FAIL" } else { "BLOCKED" }
    Write-Host "RECORDER_SUSTAINED_HIL_RESULT: $failureStatus"
    if ($null -ne $logFile) { Write-Host "LOG: $logFile" }
    if ($null -ne $healthJson) { Write-Host "HEALTH_EVIDENCE: $healthJson" }
    exit 1
}

if ($analysisExit -eq 0) {
    Write-Host "RECORDER_SUSTAINED_HIL_RESULT: PASS"
} elseif ($analysisExit -eq 2) {
    Write-Host "RECORDER_SUSTAINED_HIL_RESULT: BLOCKED"
} else {
    Write-Host "RECORDER_SUSTAINED_HIL_RESULT: FAIL"
}
Write-Host "LOG: $logFile"
Write-Host "LIVE_REFERENCE: $liveJson"
Write-Host "SAVED_REFERENCE: $savedJson"
Write-Host "HEALTH_EVIDENCE: $healthJson"

if ($analysisExit -eq 0) {
    exit 0
}
exit 1
