[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BaseUrl,

    [Parameter(Mandatory = $true)]
    [string]$ObjectId,

    [string]$LogPort = "COM6",
    [string]$ProjectPath = ".",
    [string]$BuildDir = "build",

    [ValidateRange(0, 40000)]
    [int]$ExpectedSampleRateHz = 0,

    [ValidateRange(60, 3600)]
    [int]$CaptureSeconds = 60,

    [ValidateRange(250, 10000)]
    [int]$PollIntervalMs = 1000,

    [ValidateRange(0, 60000000)]
    [uint64]$MaxRecorderOperationUs = 0
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest


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


function Get-Field {
    param(
        [AllowNull()][object]$Object,
        [string]$Name
    )

    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
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

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "" }
    try {
        return Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
    } catch {
        return ""
    }
}


function Get-StatusRate {
    param([object]$Status)
    return [int](Get-Field (Get-Field $Status "acquisition") "sample_rate_hz")
}


function Assert-MeasurementStartWorkflowReady {
    param(
        [object]$Status,
        [string]$Action
    )

    $step = [string](Get-Field (Get-Field $Status "workflow") "step")
    if ($step -notin @("RECORD_READY", "RECORDING")) {
        throw "$Action requires workflow step RECORD_READY or RECORDING; current step is '$step'. Complete the guided workflow before this HIL capture."
    }
}


function Stop-RecordingBestEffort {
    param([string]$RootUrl)
    try {
        $body = @{ action = "recording_stop" } | ConvertTo-Json -Compress
        [void](Invoke-JsonRequest ("{0}/api/control" -f $RootUrl) "POST" $body)
    } catch {
        # Preserve the primary capture error.
    }
}


function Get-MaxMatchValue {
    param(
        [object[]]$Matches,
        [string]$GroupName
    )

    [uint64]$maximum = 0
    foreach ($match in $Matches) {
        $value = [uint64]$match.Groups[$GroupName].Value
        if ($value -gt $maximum) { $maximum = $value }
    }
    return $maximum
}


$checks = [System.Collections.Generic.List[object]]::new()
$qualityObservations = [System.Collections.Generic.List[object]]::new()
$monitor = $null
$recordingMayBeActive = $false
$logFile = $null
$stderrFile = $null
$rootUrl = $BaseUrl.TrimEnd("/")
$lastStatus = $null
$beforeStatus = $null
$elapsedSeconds = 0.0
$rate = 0

try {
    $repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
    $runIdf = Join-Path $PSScriptRoot "run_idf.ps1"
    $checkPorts = Join-Path $PSScriptRoot "check_serial_ports.ps1"
    $powershell = (Get-Command powershell.exe -ErrorAction Stop).Source
    if (-not (Test-Path -LiteralPath $runIdf -PathType Leaf)) {
        throw "Missing ESP-IDF helper: $runIdf"
    }

    & $checkPorts -Ports $LogPort
    if ($LASTEXITCODE -ne 0) { throw "Serial preflight failed for $LogPort." }

    $hilDir = Join-Path $repo "build\hil"
    New-Item -ItemType Directory -Path $hilDir -Force | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $logFile = Join-Path $hilDir "acquisition-rate-$stamp.log"
    $stderrFile = Join-Path $hilDir "acquisition-rate-$stamp.err.log"
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
    $monitor = Start-Process -FilePath $powershell -ArgumentList $monitorArgs `
        -WorkingDirectory $project -RedirectStandardOutput $logFile `
        -RedirectStandardError $stderrFile -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2
    if ($monitor.HasExited) { throw "Serial monitor exited. See $stderrFile." }

    $status = Invoke-JsonRequest ("{0}/api/status" -f $rootUrl)
    $acquisition = Get-Field $status "acquisition"
    $rate = Get-StatusRate $status
    if (
        $null -eq $acquisition -or
        $acquisition.running -ne $true -or
        $acquisition.hardware_ready -ne $true -or
        $acquisition.source -ne "AD7606" -or
        $rate -notin @(5000, 10000, 20000, 40000)
    ) {
        throw "Expected running AD7606 at 5/10/20/40 kHz; /api/status reports source=$($acquisition.source), running=$($acquisition.running), rate=$rate Hz."
    }
    if ($ExpectedSampleRateHz -gt 0 -and $rate -ne $ExpectedSampleRateHz) {
        throw "Expected $ExpectedSampleRateHz Hz, but /api/status reports $rate Hz."
    }
    Add-Check $checks "Configured acquisition rate" "PASS" ("/api/status reports AD7606 at {0} Hz." -f $rate)

    if ($null -ne $status.recording -and $status.recording.running -eq $true) {
        throw "A recording is already active. Stop it before starting this capture."
    }
    Assert-MeasurementStartWorkflowReady -Status $status -Action "recording_start"
    $startBody = @{ action = "recording_start"; object_id = $ObjectId } | ConvertTo-Json -Compress
    $start = Invoke-JsonRequest ("{0}/api/control" -f $rootUrl) "POST" $startBody
    if ($start.ok -ne $true) { throw "recording_start was rejected." }
    $recordingMayBeActive = $true

    $startDeadline = (Get-Date).AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 250
        $status = Invoke-JsonRequest ("{0}/api/status" -f $rootUrl)
        if ($status.recording.running -eq $true) { break }
    } while ((Get-Date) -lt $startDeadline)
    if ($status.recording.running -ne $true) { throw "recording.running did not become true." }
    $sessionId = [string]$status.recording.session_id
    if ([string]::IsNullOrWhiteSpace($sessionId)) { throw "Recording status omitted session_id." }
    $beforeStatus = $status

    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt $CaptureSeconds) {
        $remainingMs = [int](($CaptureSeconds * 1000) - $watch.ElapsedMilliseconds)
        if ($remainingMs -gt 0) {
            Start-Sleep -Milliseconds ([math]::Min($PollIntervalMs, $remainingMs))
        }
        if ($monitor.HasExited) { throw "Serial monitor exited during capture." }
        $status = Invoke-JsonRequest ("{0}/api/status" -f $rootUrl)
        if ($status.recording.running -ne $true) { throw "Recording stopped before capture completed." }
        if ((Get-StatusRate $status) -ne $rate) { throw "Reported acquisition rate changed during capture." }
        $lastStatus = $status

        $acquisition = Get-Field $status "acquisition"
        $serial = Get-Field $acquisition "serial"
        $signal = Get-Field $acquisition "signal"
        $qualityWindow = [uint64](Get-Field $serial "quality_window_frames")
        if ($qualityWindow -ge [uint64]$rate) {
            $qualityObservations.Add([pscustomobject]@{
                Quality = [string](Get-Field $serial "quality")
                Stream = [string](Get-Field $serial "stream_state")
                Suspicious = [bool](Get-Field $serial "suspicious")
                Stuck = [bool](Get-Field $serial "stream_stuck")
                Signal = [string](Get-Field $signal "quality")
            }) | Out-Null
        }
    }
    $elapsedSeconds = $watch.Elapsed.TotalSeconds

    $stopBody = @{ action = "recording_stop" } | ConvertTo-Json -Compress
    $stop = Invoke-JsonRequest ("{0}/api/control" -f $rootUrl) "POST" $stopBody
    if ($stop.ok -ne $true) { throw "recording_stop was rejected." }
    $stopDeadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 250
        $status = Invoke-JsonRequest ("{0}/api/status" -f $rootUrl)
    } while ($status.recording.running -eq $true -and (Get-Date) -lt $stopDeadline)
    if ($status.recording.running -eq $true) { throw "Recorder did not finish within 30 seconds." }
    $recordingMayBeActive = $false
    if ([string]$status.recording.session_id -ne $sessionId) { throw "Recording session changed after stop." }
    $lastStatus = $status
    Add-Check $checks "Recording capture" "PASS" ("Captured {0:N1} seconds in session {1}." -f $elapsedSeconds, $sessionId)
} catch {
    Add-Check $checks "Runner execution" "FAIL" $_.Exception.Message
} finally {
    if ($recordingMayBeActive) { Stop-RecordingBestEffort $rootUrl }
    if ($null -ne $monitor -and -not $monitor.HasExited) {
        Stop-Process -Id $monitor.Id -Force -ErrorAction SilentlyContinue
        $monitor.WaitForExit()
    }
}

if ($null -ne $beforeStatus -and $null -ne $lastStatus) {
    $beforeAcquisition = Get-Field $beforeStatus "acquisition"
    $afterAcquisition = Get-Field $lastStatus "acquisition"
    $beforeCadence = Get-Field $beforeAcquisition "cadence"
    $afterCadence = Get-Field $afterAcquisition "cadence"
    $cadenceAvailable =
        (Get-Field $beforeCadence "available") -eq $true -and
        (Get-Field $afterCadence "available") -eq $true
    if (-not $cadenceAvailable) {
        Add-Check $checks "Acquisition cadence" "BLOCKED" "/api/status did not expose cadence counters at both capture boundaries."
    } else {
        $timerTicks = [uint64]$afterCadence.timer_ticks - [uint64]$beforeCadence.timer_ticks
        $attempts = [uint64]$afterCadence.conversion_attempts - [uint64]$beforeCadence.conversion_attempts
        $collapsed = [uint64]$afterCadence.collapsed_periods - [uint64]$beforeCadence.collapsed_periods
        $readErrors = [uint64]$afterAcquisition.read_errors - [uint64]$beforeAcquisition.read_errors
        $storedSamples = [uint64]$lastStatus.recording.samples_written
        $recorderDrops = [uint64]$lastStatus.recording.dropped_frames
        $expectedTicks = [double]$rate * $elapsedSeconds
        $observedRate = if ($elapsedSeconds -gt 0.0) { [double]$timerTicks / $elapsedSeconds } else { 0.0 }
        $rateError = [math]::Abs($observedRate - [double]$rate) / [double]$rate
        $counterInvariant = $timerTicks -eq ($attempts + $collapsed)
        $ratePass = $rateError -le 0.02 -and [math]::Abs([double]$timerTicks - $expectedTicks) -le [math]::Max(2.0, $expectedTicks * 0.02)
        $cadenceDetail = "PCNT conversions=$timerTicks attempts=$attempts collapsed_periods=$collapsed read_errors=$readErrors stored_samples=$storedSamples recorder_gaps_or_drops=$recorderDrops observed_rate=$([math]::Round($observedRate, 1))Hz target=$($rate)Hz."
        Add-Check $checks "PCNT cadence" $(if ($ratePass -and $counterInvariant) { "PASS" } else { "FAIL" }) $cadenceDetail
        Add-Check $checks "Acquisition gaps" $(if ($collapsed -eq 0 -and $readErrors -eq 0 -and $recorderDrops -eq 0) { "PASS" } else { "FAIL" }) ("Threshold=0 missed conversions/read errors/recorder gaps over {0}s; collapsed={1}, read_errors={2}, recorder_gaps_or_drops={3}." -f [math]::Round($elapsedSeconds, 1), $collapsed, $readErrors, $recorderDrops)
    }

    $serialPass = $qualityObservations.Count -gt 0
    $qualityStates = @($qualityObservations | ForEach-Object { $_.Quality } | Sort-Object -Unique)
    $streamStates = @($qualityObservations | ForEach-Object { $_.Stream } | Sort-Object -Unique)
    foreach ($sample in $qualityObservations) {
        if (
            $sample.Quality -ne "HEALTHY" -or
            $sample.Stream -notin @("NORMAL", "QUIET") -or
            $sample.Suspicious -or $sample.Stuck
        ) { $serialPass = $false }
    }
    $signalStates = @($qualityObservations | ForEach-Object { $_.Signal } | Sort-Object -Unique)
    Add-Check $checks "Serial quality" $(if ($serialPass) { "PASS" } else { "FAIL" }) ("Mature windows={0}; transport={1}; stream={2}; signal={3}." -f $qualityObservations.Count, ($qualityStates -join ","), ($streamStates -join ","), ($signalStates -join ","))
}

$logContent = (Read-TextFile $logFile) + "`n" + (Read-TextFile $stderrFile)
$writes = @([regex]::Matches(
    $logContent,
    '(?im)RECORDER_IO_TIMING\s+op=fwrite\s+requested=(?<requested>\d+)\s+written=(?<written>\d+)\s+windows=(?<windows>\d+)\s+max_window_us=(?<window>\d+)\s+fwrite_us=(?<fwrite>\d+)\s+operation_us=(?<operation>\d+)\s+result=(?<result>-?\d+)'
))
$stopMarkers = @([regex]::Matches(
    $logContent,
    '(?im)STOP_COMPLETE\s+stage=(?<stage>[a-z_]+).*?samples=(?<samples>\d+)\s+drops=(?<drops>\d+)\s+err=(?<error>[A-Z0-9_]+)'
))
$cacheDrops = @([regex]::Matches($logContent, '(?im)RECORDER_CACHE\s+[^\r\n]*?total_dropped=(?<drops>\d+)'))
if ($writes.Count -eq 0) {
    Add-Check $checks "SD write latency" "BLOCKED" "No RECORDER_IO_TIMING op=fwrite samples were captured."
} else {
    $maxOperationUs = [uint64]0
    $maxWriteUs = [uint64]0
    $writeErrors = [System.Collections.Generic.List[string]]::new()
    foreach ($write in $writes) {
        $operationUs = [uint64]$write.Groups["operation"].Value
        $writeUs = [uint64]$write.Groups["fwrite"].Value
        if ($operationUs -gt $maxOperationUs) { $maxOperationUs = $operationUs }
        if ($writeUs -gt $maxWriteUs) { $maxWriteUs = $writeUs }
        if (
            [int]$write.Groups["result"].Value -ne 0 -or
            [uint64]$write.Groups["written"].Value -ne [uint64]$write.Groups["requested"].Value
        ) { $writeErrors.Add($write.Value.Trim()) }
    }
    $latencyPass = $writeErrors.Count -eq 0 -and ($MaxRecorderOperationUs -eq 0 -or $maxOperationUs -le $MaxRecorderOperationUs)
    $limitText = if ($MaxRecorderOperationUs -gt 0) { "limit=${MaxRecorderOperationUs}us" } else { "no latency limit supplied; measurement only" }
    Add-Check $checks "SD write latency" $(if ($latencyPass) { "PASS" } else { "FAIL" }) ("writes={0} max_fwrite_us={1} max_operation_us={2} max_storage_window_us={3} {4}; write_errors={5}." -f $writes.Count, $maxWriteUs, $maxOperationUs, (Get-MaxMatchValue $writes "window"), $limitText, $writeErrors.Count)
}

if ($stopMarkers.Count -gt 0) {
    $stop = $stopMarkers[-1]
    Add-Check $checks "Recorder stop log" $(if ($stop.Groups["stage"].Value -eq "complete" -and $stop.Groups["error"].Value -eq "ESP_OK") { "PASS" } else { "FAIL" }) ("STOP_COMPLETE stage={0} samples={1} drops={2} err={3}." -f $stop.Groups["stage"].Value, $stop.Groups["samples"].Value, $stop.Groups["drops"].Value, $stop.Groups["error"].Value)
    if ($null -ne $lastStatus -and [string]$lastStatus.recording.dropped_frames -ne $stop.Groups["drops"].Value) {
        Add-Check $checks "Recorder gap log reconciliation" "FAIL" ("/api/status dropped_frames={0}, STOP_COMPLETE drops={1}." -f $lastStatus.recording.dropped_frames, $stop.Groups["drops"].Value)
    } elseif ($cacheDrops.Count -gt 0 -and [uint64]$cacheDrops[-1].Groups["drops"].Value -gt [uint64]$stop.Groups["drops"].Value) {
        Add-Check $checks "Recorder gap log reconciliation" "FAIL" ("RECORDER_CACHE total_dropped={0} exceeds STOP_COMPLETE drops={1}." -f $cacheDrops[-1].Groups["drops"].Value, $stop.Groups["drops"].Value)
    } elseif ($cacheDrops.Count -gt 0) {
        Add-Check $checks "Recorder gap log reconciliation" "PASS" ("STOP_COMPLETE final missing samples={0}; latest RECORDER_CACHE progress={1}." -f $stop.Groups["drops"].Value, $cacheDrops[-1].Groups["drops"].Value)
    } else {
        Add-Check $checks "Recorder gap log reconciliation" "SKIPPED" "No RECORDER_CACHE total_dropped line was captured."
    }
} else {
    Add-Check $checks "Recorder stop log" "BLOCKED" "No STOP_COMPLETE marker was captured."
}

$cpuSamples = @([regex]::Matches($logContent, '(?im)RUNTIME_CPU\s+[^\r\n]*?cpu1_busy=(?<whole>\d+)\.(?<fraction>\d+)'))
if ($cpuSamples.Count -gt 0) {
    $maximumCpu = 0.0
    foreach ($sample in $cpuSamples) {
        $value = [double]("{0}.{1}" -f $sample.Groups["whole"].Value, $sample.Groups["fraction"].Value)
        if ($value -gt $maximumCpu) { $maximumCpu = $value }
    }
    Write-Host ("Core-1 aggregate CPU busy: max={0:N1}% from RUNTIME_CPU logs; per-task CPU is not included in /api/status." -f $maximumCpu)
} else {
    Write-Host "Core-1 load: unavailable; /api/status exposes task heartbeats, not CPU, and no RUNTIME_CPU log was captured."
}

$workflowHwm = [regex]::Matches(
    $logContent,
    '(?im)poll task STACK_HWM\s+phase=(?<phase>[a-z0-9-]+)\s+free=(?<free>\d+)\s+of\s+(?<total>\d+)\s+bytes\s+polls=(?<polls>\d+)'
)
if ($workflowHwm.Count -gt 0) {
    $hwm = $workflowHwm[-1]
    Write-Host ("workflow_poll STACK_HWM: phase={0} free={1}/{2} bytes polls={3}." -f $hwm.Groups["phase"].Value, $hwm.Groups["free"].Value, $hwm.Groups["total"].Value, $hwm.Groups["polls"].Value)
} else {
    Write-Host "workflow_poll STACK_HWM: not observed; firmware emits it after 100 polls or the first workflow calculation."
}

$reportedConfig = [regex]::Match($logContent, '(?im)AD7606[_ ]?ACQ:.*Started:\s*(?<rate>\d+)\s*Hz,\s*4\s*channels,\s*SPI3=(?<sclk>\d+)\s*Hz,\s*OS(?<os>\d+),\s*mode=(?<mode>[a-z_]+)')
if ($reportedConfig.Success) {
    Write-Host ("Startup config: rate={0}Hz SCLK={1}Hz OS{2} mode={3}. Confirm OS Kconfig matches the physical jumpers." -f $reportedConfig.Groups["rate"].Value, $reportedConfig.Groups["sclk"].Value, $reportedConfig.Groups["os"].Value, $reportedConfig.Groups["mode"].Value)
} else {
    Write-Host "Startup config: AD7606 rate/SCLK/OS/read-mode marker not captured."
}

$failed = @($checks | Where-Object { $_.Status -eq "FAIL" -or $_.Status -eq "BLOCKED" })
$result = if ($failed.Count -eq 0) { "PASS" } else { "FAIL" }
Write-Host "===== Acquisition rate HIL report ====="
Write-Host ("{0,-34} {1,-8} {2}" -f "CHECK", "STATUS", "DETAIL")
Write-Host ("{0,-34} {1,-8} {2}" -f "-----", "------", "------")
foreach ($check in $checks) {
    Write-Host ("{0,-34} {1,-8} {2}" -f $check.Name, $check.Status, $check.Detail)
}
if ($logFile) { Write-Host ("LOG_FILE: {0}" -f $logFile) }
Write-Host ("RATE_HZ: {0} CAPTURE_SECONDS: {1} GAP_LIMIT: 0" -f $rate, [math]::Round($elapsedSeconds, 1))
Write-Host ("ACQUISITION_RATE_HIL_RESULT: {0}" -f $result)

if ($result -eq "PASS") { exit 0 }
exit 1
