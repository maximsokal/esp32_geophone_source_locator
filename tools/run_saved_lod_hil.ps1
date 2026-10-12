[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BaseUrl,

    [Parameter(Mandatory = $true)]
    [string]$ObjectId,

    [string]$LogPort = "COM6",
    [string]$ProjectPath = ".",
    [string]$BuildDir = "build",

    [ValidateRange(64, 1000000)]
    [int]$TargetFrames = 1024,

    [ValidateRange(16, 1200)]
    [int]$Points = 16,

    [ValidateRange(5, 180)]
    [int]$TimeoutSeconds = 60,

    [ValidateRange(100, 5000)]
    [int]$PollIntervalMs = 250,

    [ValidateRange(0, 40000)]
    [int]$ExpectedSampleRateHz = 0
)

$ErrorActionPreference = "Stop"


function Add-Check {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Name,
        [ValidateSet("PASS", "FAIL", "BLOCKED", "SKIPPED")]
        [string]$Status,
        [string]$Detail
    )

    $Checks.Add([pscustomobject]@{
        Name = $Name
        Status = $Status
        Detail = $Detail
    }) | Out-Null
}


function Invoke-JsonRequest {
    param(
        [string]$Uri,
        [ValidateSet("GET", "POST")]
        [string]$Method = "GET",
        [string]$Body,
        [int]$TimeoutSec = 10
    )

    $params = @{
        Uri = $Uri
        Method = $Method
        TimeoutSec = $TimeoutSec
        ErrorAction = "Stop"
    }
    if (-not [string]::IsNullOrWhiteSpace($Body)) {
        $params.Body = $Body
        $params.ContentType = "application/json"
    }
    return Invoke-RestMethod @params
}


function Read-TextFile {
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


function Wait-RecordingState {
    param(
        [string]$RootUrl,
        [bool]$Running,
        [datetime]$Deadline
    )

    $status = $null
    do {
        $status = Invoke-JsonRequest ("{0}/api/status" -f $RootUrl)
        if ($null -ne $status.recording -and $status.recording.running -eq $Running) {
            return $status
        }
        if ((Get-Date) -lt $Deadline) {
            Start-Sleep -Milliseconds $PollIntervalMs
        }
    } while ((Get-Date) -lt $Deadline)

    throw ("Timed out waiting for recording.running={0}." -f $Running)
}


function Assert-MeasurementStartWorkflowReady {
    param(
        [string]$Base,
        [string]$Action
    )

    $status = Invoke-JsonRequest ("{0}/api/status" -f $Base)
    $step = [string]$status.workflow.step
    $guidedSteps = @("CALIB_RESULT", "PLACEMENT", "GPS_CHOICE", "GPS_PENDING", "LAYOUT_READY", "RANGING", "RANGING_RESULT", "SCAN_CHOICE", "SCANNING")
    if ($step -in $guidedSteps) {
        throw "$Action is waiting for the guided workflow at '$step'. Confirm placement, GPS choice, layout calculation, and scan choice before this HIL run."
    }
    if ($step -notin @("RECORD_READY", "RECORDING")) {
        throw "$Action requires workflow step RECORD_READY or RECORDING; current step is '$step'. Complete the guided workflow before this HIL run."
    }
}


function Invoke-WorkflowRecordAgain {
    param(
        [string]$Base,
        [int]$TimeoutSec,
        [int]$PollMs
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $status = Invoke-JsonRequest ("{0}/api/status" -f $Base)
        $workflow = $status.workflow
        $step = [string]$workflow.step
        if ($step -eq "RECORD_READY") { return }
        if ($step -eq "STOPPED") {
            $body = @{
                action = "workflow_command"
                command = "RECORD_AGAIN"
                generation = $workflow.generation
            } | ConvertTo-Json -Compress
            $result = Invoke-JsonRequest ("{0}/api/control" -f $Base) "POST" $body
            if ($result.ok -eq $true) { return }
            throw "workflow RECORD_AGAIN rejected: $($result.reason)"
        }
        if ($step -ne "RECORDING") {
            throw "workflow RECORD_AGAIN requires STOPPED or RECORD_READY; current step is '$step'."
        }
        if ((Get-Date) -lt $deadline) { Start-Sleep -Milliseconds $PollMs }
    } while ((Get-Date) -lt $deadline)

    throw "workflow did not become STOPPED or RECORD_READY within ${TimeoutSec}s after recording stop."
}


function Stop-RecordingBestEffort {
    param([string]$RootUrl)

    try {
        $body = @{ action = "recording_stop" } | ConvertTo-Json -Compress
        [void](Invoke-JsonRequest ("{0}/api/control" -f $RootUrl) "POST" $body)
    } catch {
        # Keep the original HIL failure as the primary diagnostic.
    }
}


function Wait-LodMarker {
    param(
        [string]$LogFile,
        [string]$SessionId,
        [int]$Frames,
        [int]$PointCount,
        [datetime]$Deadline
    )

    $escapedSession = [regex]::Escape($SessionId)
    $expectedRecords = [int]($Frames / 64)
    $pattern = (
        "WAVEFORM_READ_PATH\s+source=lod64\s+session={0}\s+start=0\s+frames={1}\s+points={2}\s+raw_frames=0\s+lod_records={3}\s+lod_frames={1}" -f
        $escapedSession,
        $Frames,
        $PointCount,
        $expectedRecords
    )

    do {
        $content = Read-TextFile $LogFile
        if ($content -match $pattern) {
            return $true
        }
        if ((Get-Date) -lt $Deadline) {
            Start-Sleep -Milliseconds $PollIntervalMs
        }
    } while ((Get-Date) -lt $Deadline)

    return $false
}


$checks = [System.Collections.Generic.List[object]]::new()
$monitor = $null
$logFile = $null
$stderrFile = $null
$recordingMayBeActive = $false
$rootUrl = $BaseUrl.TrimEnd("/")

try {
    if ($TargetFrames % 64 -ne 0) {
        throw "-TargetFrames must be divisible by the LOD64 block size (64)."
    }
    if ($TargetFrames % $Points -ne 0) {
        throw "-TargetFrames must be divisible by -Points so output buckets are exact."
    }
    $bucketFrames = [int]($TargetFrames / $Points)
    if ($bucketFrames -lt 64 -or $bucketFrames % 64 -ne 0) {
        throw "Each output bucket must contain an integer multiple of 64 frames; choose TargetFrames/Points accordingly."
    }

    $repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
    $runIdf = Join-Path $PSScriptRoot "run_idf.ps1"
    $checkPorts = Join-Path $PSScriptRoot "check_serial_ports.ps1"
    $powershell = (Get-Command powershell.exe -ErrorAction Stop).Source

    if (-not (Test-Path -LiteralPath $runIdf -PathType Leaf)) {
        throw "Missing ESP-IDF helper: $runIdf"
    }
    if (-not (Test-Path -LiteralPath $checkPorts -PathType Leaf)) {
        throw "Missing serial-port helper: $checkPorts"
    }

    & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $checkPorts -Ports $LogPort
    if ($LASTEXITCODE -ne 0) {
        throw "Serial preflight failed for $LogPort."
    }
    Add-Check $checks "Serial preflight" "PASS" ("Live serial port detected: {0}." -f $LogPort)

    $hilDir = Join-Path $repo "build\hil"
    New-Item -ItemType Directory -Path $hilDir -Force | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $logFile = Join-Path $hilDir "saved-lod-$stamp.log"
    $stderrFile = Join-Path $hilDir "saved-lod-$stamp.err.log"

    $project = if ([IO.Path]::IsPathRooted($ProjectPath)) {
        (Resolve-Path -LiteralPath $ProjectPath).Path
    } else {
        (Resolve-Path -LiteralPath (Join-Path $repo $ProjectPath)).Path
    }

    $monitorArgs = @(
        "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"{0}"' -f $runIdf),
        "-Action", "monitor", "-ProjectPath", $ProjectPath,
        "-BuildDir", $BuildDir, "-Port", $LogPort
    )
    $monitor = Start-Process `
        -FilePath $powershell `
        -ArgumentList $monitorArgs `
        -WorkingDirectory $project `
        -RedirectStandardOutput $logFile `
        -RedirectStandardError $stderrFile `
        -PassThru `
        -WindowStyle Hidden

    Start-Sleep -Seconds 2
    if ($monitor.HasExited) {
        $monitorError = Read-TextFile $stderrFile
        throw "Serial monitor exited immediately. Close every ESP-IDF/VS Code serial monitor that owns $LogPort and retry. $monitorError"
    }
    Add-Check $checks "Serial monitor" "PASS" ("Capturing target output to {0}." -f $logFile)

    $status = Invoke-JsonRequest ("{0}/api/status" -f $rootUrl)
    if ($null -eq $status.acquisition -or $status.acquisition.running -ne $true) {
        throw "Acquisition is not running according to /api/status."
    }
    if ($status.acquisition.source -ne "AD7606") {
        throw "Expected AD7606 acquisition, got '$($status.acquisition.source)'."
    }
    $reportedSampleRateHz = [int]$status.acquisition.sample_rate_hz
    if ($reportedSampleRateHz -le 0) {
        throw "Acquisition reported an invalid sample rate: $reportedSampleRateHz Hz."
    }
    if ($ExpectedSampleRateHz -gt 0 -and $reportedSampleRateHz -ne $ExpectedSampleRateHz) {
        throw "Expected $ExpectedSampleRateHz Hz acquisition, got $reportedSampleRateHz Hz."
    }
    if ($null -ne $status.recording -and $status.recording.running -eq $true) {
        throw "A recording is already active. Stop it before running this HIL test."
    }
    Add-Check $checks "Acquisition prerequisite" "PASS" ("AD7606 reports {0} Hz." -f $reportedSampleRateHz)

    Assert-MeasurementStartWorkflowReady -Base $rootUrl -Action "recording_start"
    $startBody = @{ action = "recording_start"; object_id = $ObjectId } | ConvertTo-Json -Compress
    $start = Invoke-JsonRequest ("{0}/api/control" -f $rootUrl) "POST" $startBody
    if ($start.ok -ne $true) {
        throw "recording_start for the LOD source session was rejected."
    }
    $recordingMayBeActive = $true

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $status = Wait-RecordingState $rootUrl $true $deadline
    $sourceSessionId = [string]$status.recording.session_id
    if ([string]::IsNullOrWhiteSpace($sourceSessionId)) {
        throw "The source recording became active without a session_id."
    }

    do {
        if ($monitor.HasExited) {
            throw "Serial monitor exited while recording the LOD source session."
        }
        $status = Invoke-JsonRequest ("{0}/api/status" -f $rootUrl)
        if ([uint64]$status.recording.dropped_frames -ne 0) {
            throw "Recorder reported dropped_frames=$($status.recording.dropped_frames) before reaching the LOD target."
        }
        if ([uint64]$status.recording.samples_written -ge [uint64]$TargetFrames) {
            break
        }
        Start-Sleep -Milliseconds $PollIntervalMs
    } while ((Get-Date) -lt $deadline)

    if ([uint64]$status.recording.samples_written -lt [uint64]$TargetFrames) {
        throw "Timed out before samples_written reached the LOD target of $TargetFrames frames."
    }
    Add-Check $checks "LOD source recording" "PASS" ("Session {0} reached samples_written={1}, dropped_frames=0." -f $sourceSessionId, $status.recording.samples_written)

    $stopBody = @{ action = "recording_stop" } | ConvertTo-Json -Compress
    $stop = Invoke-JsonRequest ("{0}/api/control" -f $rootUrl) "POST" $stopBody
    if ($stop.ok -ne $true) {
        throw "recording_stop for the LOD source session was rejected."
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $status = Wait-RecordingState $rootUrl $false $deadline
    $recordingMayBeActive = $false
    if ([string]$status.recording.session_id -ne $sourceSessionId) {
        throw "Source session_id changed after stop."
    }
    if ([uint64]$status.recording.samples_written -lt [uint64]$TargetFrames) {
        throw "Stopped source session contains fewer than $TargetFrames persisted frames."
    }
    if ([uint64]$status.recording.dropped_frames -ne 0) {
        throw "Stopped source session has dropped_frames=$($status.recording.dropped_frames)."
    }
    Add-Check $checks "LOD source finalized" "PASS" ("Session {0} finalized with {1} persisted frames." -f $sourceSessionId, $status.recording.samples_written)

    # Starting another session deliberately resets the recorder PSRAM cache.
    # The following saved-session request therefore cannot pass through the
    # just-stopped session's cache fast path. With the exact aligned range below,
    # every 64-frame LOD block maps to one output bucket, so source=lod64 proves
    # actual sidecar payload use rather than merely sidecar availability.
    Invoke-WorkflowRecordAgain -Base $rootUrl -TimeoutSec $TimeoutSeconds -PollMs $PollIntervalMs
    $evictStart = Invoke-JsonRequest ("{0}/api/control" -f $rootUrl) "POST" $startBody
    if ($evictStart.ok -ne $true) {
        throw "recording_start for the cache-eviction session was rejected."
    }
    $recordingMayBeActive = $true

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $status = Wait-RecordingState $rootUrl $true $deadline
    $evictionSessionId = [string]$status.recording.session_id
    if ([string]::IsNullOrWhiteSpace($evictionSessionId) -or $evictionSessionId -eq $sourceSessionId) {
        throw "Cache-eviction recording did not create a distinct session."
    }
    Add-Check $checks "PSRAM cache eviction" "PASS" ("Active session changed from {0} to {1}." -f $sourceSessionId, $evictionSessionId)

    $encodedObject = [uri]::EscapeDataString($ObjectId)
    $encodedSession = [uri]::EscapeDataString($sourceSessionId)
    $savedUri = (
        "{0}/api/seismic/session?object_id={1}&session_id={2}&start_frame=0&frame_count={3}&points={4}" -f
        $rootUrl,
        $encodedObject,
        $encodedSession,
        $TargetFrames,
        $Points
    )
    $saved = Invoke-JsonRequest $savedUri

    if ($saved.object_id -ne $ObjectId) {
        throw "Saved waveform returned object_id '$($saved.object_id)', expected '$ObjectId'."
    }
    if ($saved.session_id -ne $sourceSessionId) {
        throw "Saved waveform returned session_id '$($saved.session_id)', expected '$sourceSessionId'."
    }
    if ($saved.complete -ne $true) {
        throw "Saved waveform did not report complete=true."
    }
    if ([int]$saved.sample_rate_hz -ne $reportedSampleRateHz) {
        throw "Saved waveform sample_rate_hz=$($saved.sample_rate_hz), expected reported acquisition rate $reportedSampleRateHz."
    }
    if ([uint64]$saved.start_frame -ne 0 -or [uint64]$saved.frame_count -ne [uint64]$TargetFrames) {
        throw "Saved waveform returned start_frame=$($saved.start_frame), frame_count=$($saved.frame_count); expected 0/$TargetFrames."
    }
    if ([int]$saved.count -ne $Points -or @($saved.points).Count -ne $Points) {
        throw "Saved waveform returned count=$($saved.count) and $(@($saved.points).Count) point objects; expected $Points."
    }
    Add-Check $checks "Saved waveform HTTP" "PASS" ("Read {0} frames as {1} exact min/max buckets while a different recording was active." -f $TargetFrames, $Points)

    Stop-RecordingBestEffort $rootUrl
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    [void](Wait-RecordingState $rootUrl $false $deadline)
    $recordingMayBeActive = $false

    $deadline = (Get-Date).AddSeconds([math]::Min($TimeoutSeconds, 15))
    $expectedLodRecords = [int]($TargetFrames / 64)
    if (Wait-LodMarker $logFile $sourceSessionId $TargetFrames $Points $deadline) {
        Add-Check $checks "LOD64 read-path marker" "PASS" ("Observed source=lod64 with raw_frames=0, lod_records={0}, lod_frames={1} for session={2}." -f $expectedLodRecords, $TargetFrames, $sourceSessionId)
    } else {
        $content = (Read-TextFile $logFile) + "`n" + (Read-TextFile $stderrFile)
        $fallback = [regex]::Match(
            $content,
            ("WAVEFORM_READ_PATH\s+source=(cache|raw|lod64)\s+session={0}\s+start=0\s+frames={1}\s+points={2}[^\r\n]*" -f [regex]::Escape($sourceSessionId), $TargetFrames, $Points)
        )
        if ($fallback.Success) {
            throw "Saved waveform marker did not prove the exact LOD64 payload contract: $($fallback.Value.Trim())"
        }
        throw "No matching WAVEFORM_READ_PATH marker was observed. The firmware may be older than the read-path instrumentation or the sidecar was unavailable/invalid."
    }

    $combinedLog = (Read-TextFile $logFile) + "`n" + (Read-TextFile $stderrFile)
    $panicPatterns = @("Guru Meditation", "Backtrace:", "CORRUPT HEAP", "panic'ed")
    $panic = $panicPatterns | Where-Object { $combinedLog -match $_ } | Select-Object -First 1
    if ($null -eq $panic) {
        Add-Check $checks "Panic detection" "PASS" "No panic marker observed during the LOD64 exercise."
    } else {
        throw "Panic marker observed during the LOD64 exercise: $panic"
    }
} catch {
    Add-Check $checks "Runner execution" "FAIL" $_.Exception.Message
} finally {
    if ($recordingMayBeActive) {
        Stop-RecordingBestEffort $rootUrl
    }
    if ($null -ne $monitor -and -not $monitor.HasExited) {
        Stop-Process -Id $monitor.Id -Force -ErrorAction SilentlyContinue
        $monitor.WaitForExit()
    }
}

$failed = @($checks | Where-Object { $_.Status -eq "FAIL" -or $_.Status -eq "BLOCKED" })
$result = if ($failed.Count -eq 0) { "PASS" } else { "FAIL" }

Write-Host "===== Saved waveform LOD64 HIL report ====="
Write-Host ("{0,-30} {1,-8} {2}" -f "CHECK", "STATUS", "DETAIL")
Write-Host ("{0,-30} {1,-8} {2}" -f "-----", "------", "------")
foreach ($check in $checks) {
    Write-Host ("{0,-30} {1,-8} {2}" -f $check.Name, $check.Status, $check.Detail)
}
if (-not [string]::IsNullOrWhiteSpace($logFile)) {
    Write-Host ("LOG_FILE: {0}" -f $logFile)
}
Write-Host "CONTRACT: a distinct second recording invalidates the first session's PSRAM-cache path; aligned TargetFrames/Points geometry makes every 64-frame block eligible for LOD64. PASS additionally requires raw_frames=0, lod_records=TargetFrames/64 and lod_frames=TargetFrames, so sidecar payload consumption is proven rather than inferred from sidecar presence."
Write-Host ("SAVED_LOD_HIL_RESULT: {0}" -f $result)

if ($result -eq "PASS") {
    exit 0
}
exit 1
