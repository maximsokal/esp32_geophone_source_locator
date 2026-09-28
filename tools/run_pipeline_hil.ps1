[CmdletBinding()]
param(
    [string]$FlashPort = "COM4",
    [string]$LogPort = "COM6",
    [string]$ProjectPath = ".",
    [string]$BuildDir = "build",
    [switch]$SkipFlash,
    [string]$LogPath,
    [string]$BaseUrl,
    [switch]$Record,
    [string]$ObjectId,
    [switch]$RequireFullPipeline,
    [switch]$ConfirmTftRendering,
    [switch]$ConfirmKeypadResponsive,
    [switch]$ConfirmPowerButtonResponsive,
    [switch]$ConfirmPowerButtonTiming,
    [ValidateRange(0, 60000)]
    [int]$PowerButtonTimingMs = 0,
    [ValidateRange(0, 60000)]
    [int]$PowerButtonTimingTargetMs = 0,
    [int]$PowerButtonTimingToleranceMs = -1,

    [ValidateRange(5, 900)]
    [int]$TimeoutSeconds = 120,

    [ValidateRange(100, 10000)]
    [int]$PollIntervalMs = 500,

    [ValidateRange(0, 40000)]
    [int]$ExpectedSampleRateHz = 0,

    [ValidateRange(100, 60000)]
    [int]$MaxTaskHeartbeatAgeMs = 1000
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


function Add-PhysicalObservationCheck {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Name,
        [bool]$Confirmed,
        [bool]$Replay,
        [bool]$RequireFull,
        [string]$LiveObservation,
        [string]$ReplayObservation,
        [string]$SwitchName
    )

    if ($Replay -and $Confirmed) {
        Add-Check $Checks $Name "BLOCKED" ("-{0} cannot be used with -LogPath: a captured log cannot prove physical behavior." -f $SwitchName)
    } elseif ($Confirmed -and -not $RequireFull) {
        Add-Check $Checks $Name "BLOCKED" ("-{0} requires -RequireFullPipeline so API and SD evidence cannot be omitted." -f $SwitchName)
    } elseif ($Replay) {
        $status = if ($RequireFull) { "BLOCKED" } else { "SKIPPED" }
        Add-Check $Checks $Name $status $ReplayObservation
    } elseif ($Confirmed) {
        Add-Check $Checks $Name "PASS" ("Manual HIL attestation supplied: {0}" -f $LiveObservation)
    } elseif ($RequireFull) {
        Add-Check $Checks $Name "BLOCKED" ("Observe the live target, then rerun with -{0}; {1}" -f $SwitchName, $LiveObservation)
    } else {
        Add-Check $Checks $Name "SKIPPED" $LiveObservation
    }
}


function Add-PowerButtonTimingCheck {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [bool]$Confirmed,
        [bool]$Replay,
        [bool]$RequireFull,
        [int]$MeasuredMs,
        [int]$TargetMs,
        [int]$ToleranceMs
    )

    $name = "Power button raw timing"
    $measurementInstruction = "Measure raw GPIO21 HIGH to the authoritative shutdown latch, then pass -ConfirmPowerButtonTiming -PowerButtonTimingMs <measured> -PowerButtonTimingTargetMs <target> -PowerButtonTimingToleranceMs <tolerance>."

    if ($Replay -and $Confirmed) {
        Add-Check $Checks $name "BLOCKED" "-ConfirmPowerButtonTiming cannot be used with -LogPath: a captured log cannot prove a raw GPIO21 timing measurement."
    } elseif ($Confirmed -and -not $RequireFull) {
        Add-Check $Checks $name "BLOCKED" "-ConfirmPowerButtonTiming requires -RequireFullPipeline so the timing attestation is attached to a complete live run."
    } elseif ($Replay) {
        $status = if ($RequireFull) { "BLOCKED" } else { "SKIPPED" }
        Add-Check $Checks $name $status "Serial replay cannot prove raw GPIO21 HIGH to shutdown-latch timing."
    } elseif (-not $Confirmed) {
        $status = if ($RequireFull) { "BLOCKED" } else { "SKIPPED" }
        Add-Check $Checks $name $status $measurementInstruction
    } elseif ($MeasuredMs -le 0 -or $TargetMs -le 0 -or $ToleranceMs -lt 0) {
        Add-Check $Checks $name "BLOCKED" "The timing attestation requires positive measured/target milliseconds and an explicit non-negative tolerance."
    } else {
        $difference = [math]::Abs($MeasuredMs - $TargetMs)
        $detail = "Measured GPIO21 HIGH -> shutdown latch={0} ms; target={1} ms +/- {2} ms." -f $MeasuredMs, $TargetMs, $ToleranceMs
        if ($difference -le $ToleranceMs) {
            Add-Check $Checks $name "PASS" ($detail + " Within tolerance.")
        } else {
            Add-Check $Checks $name "FAIL" ($detail + " Outside tolerance.")
        }
    }
}


function Resolve-RepoPath {
    param([string]$InputPath)

    $repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
    $candidate = if ([IO.Path]::IsPathRooted($InputPath)) {
        $InputPath
    } else {
        Join-Path $repo $InputPath
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}


function Read-PipelineLog {
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


function Get-PipelineReportedSampleRate {
    param([string]$Content)

    $marker = [regex]::Match(
        $Content,
        "(?im)AD7606[_ ]?ACQ:.*Started:\s*(?<rate>\d+)\s*Hz,\s*8\s*channels,\s*SPI3 dedicated,\s*core=1"
    )
    if (-not $marker.Success) {
        return 0
    }
    return [int]$marker.Groups["rate"].Value
}


function Add-PowerButtonTimingMarkerCheck {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Content
    )

    $marker = [regex]::Match(
        $Content,
        "(?im)POWER_BUTTON_TIMING:\s*debounced_down_ms=(\d+)\s+latch_ms=(\d+)\s+debounced_elapsed_ms=(\d+)\s+configured_long_press_ms=(\d+)"
    )
    if ($marker.Success) {
        [uint64]$debouncedDownMs = $marker.Groups[1].Value
        [uint64]$latchMs = $marker.Groups[2].Value
        [uint64]$reportedElapsedMs = $marker.Groups[3].Value
        [uint64]$configuredLongPressMs = $marker.Groups[4].Value

        if ($latchMs -lt $debouncedDownMs) {
            Add-Check $Checks "Power button timing marker" "FAIL" "POWER_BUTTON_TIMING has latch_ms earlier than debounced_down_ms."
        } elseif (($latchMs - $debouncedDownMs) -ne $reportedElapsedMs) {
            Add-Check $Checks "Power button timing marker" "FAIL" "POWER_BUTTON_TIMING debounced_elapsed_ms does not match latch_ms - debounced_down_ms."
        } else {
            Add-Check $Checks "Power button timing marker" "PASS" ("debounced_elapsed_ms={0} ms; configured_long_press_ms={1} ms." -f $reportedElapsedMs, $configuredLongPressMs)
        }
    } elseif ($Content -match "(?im)POWER_BUTTON[_ ]?TIMING") {
        Add-Check $Checks "Power button timing marker" "FAIL" "POWER_BUTTON_TIMING was present but malformed or incomplete."
    } else {
        Add-Check $Checks "Power button timing marker" "SKIPPED" "No debounced power-button timing marker was observed."
    }
}


function Test-PipelineLog {
    param(
        [string]$Content,
        [int]$ExpectedRate = 0
    )

    $checks = [System.Collections.Generic.List[object]]::new()
    if ([string]::IsNullOrWhiteSpace($Content)) {
        Add-Check $checks "Serial log" "FAIL" "The log is empty or could not be read."
        Add-PowerButtonTimingMarkerCheck $checks $Content
        return $checks
    }

    $markers = @(
        @("AD7606 GPIO configuration", "AD7606[_ ]?DRIVER:.*GPIO configured"),
        @("AD7606 SPI3 initialization", "AD7606[_ ]?DRIVER:.*SPI3 initialized successfully.*DMA[=: ]+auto"),
        @("AD7606 self-test", "AD7606[_ ]?DRIVER:.*Self-test conversion \+ SPI read OK")
    )

    foreach ($marker in $markers) {
        if ($Content -match $marker[1]) {
            Add-Check $checks $marker[0] "PASS" "Marker observed."
        } else {
            Add-Check $checks $marker[0] "FAIL" "Required marker was not observed."
        }
    }

    $reportedRate = Get-PipelineReportedSampleRate $Content
    if ($reportedRate -le 0) {
        Add-Check $checks "AD7606 acquisition started" "FAIL" "No valid reported acquisition sample rate was observed."
    } elseif ($ExpectedRate -gt 0 -and $reportedRate -ne $ExpectedRate) {
        Add-Check $checks "AD7606 acquisition started" "FAIL" ("Reported rate is {0}Hz, expected {1}Hz." -f $reportedRate, $ExpectedRate)
    } else {
        Add-Check $checks "AD7606 acquisition started" "PASS" ("Startup marker reports {0}Hz." -f $reportedRate)
    }

    $diagnosticChannels = @(
        [regex]::Matches(
            $Content,
            "(?im)AD7606[_ ]?DIAG:\s*CH([1-8])\s+n=([1-9]\d*)"
        ) |
            ForEach-Object { [int]$_.Groups[1].Value } |
            Sort-Object -Unique
    )
    $missingDiagnosticChannels = @(
        1..8 | Where-Object { $_ -notin $diagnosticChannels }
    )
    if ($missingDiagnosticChannels.Count -eq 0) {
        Add-Check $checks "AD7606 channel diagnostics" "PASS" ("Positive-sample diagnostics observed for CH1-CH8 (channels: {0})." -f ($diagnosticChannels -join ", "))
    } else {
        $shown = if ($diagnosticChannels.Count -eq 0) { "none" } else { $diagnosticChannels -join ", " }
        Add-Check $checks "AD7606 channel diagnostics" "FAIL" ("Expected positive-sample diagnostics for CH1-CH8; observed: {0}; missing: {1}." -f $shown, ($missingDiagnosticChannels -join ", "))
    }

    $panicPatterns = @(
        "Guru Meditation",
        "abort\(\)",
        "Backtrace:",
        "CORRUPT HEAP",
        "Stack smashing protect failure",
        "panic'ed"
    )
    $panicMatch = $panicPatterns | Where-Object { $Content -match $_ } | Select-Object -First 1
    if ($null -eq $panicMatch) {
        Add-Check $checks "Panic detection" "PASS" "No panic marker observed."
    } else {
        Add-Check $checks "Panic detection" "FAIL" ("Panic marker observed: {0}." -f $panicMatch)
    }

    $resetCount = @(
        [regex]::Matches($Content, "(?im)^\s*rst:\s*0x[0-9a-f]+")
    ).Count
    if ($resetCount -le 1) {
        Add-Check $checks "Unexpected reset detection" "PASS" ("Observed {0} boot reset marker; no repeated reset loop." -f $resetCount)
    } else {
        Add-Check $checks "Unexpected reset detection" "FAIL" ("Observed {0} reset markers; repeated reboot suspected." -f $resetCount)
    }

    $readFailure = [regex]::Match(
        $Content,
        "(?im)(AD7606[_ ]?ACQ:.*Read failed|Seismic buffer push failed)"
    )
    if (-not $readFailure.Success) {
        Add-Check $checks "Acquisition read failures" "PASS" "No AD7606 read or buffer-publish failure observed."
    } else {
        Add-Check $checks "Acquisition read failures" "FAIL" ("Failure marker observed: {0}." -f $readFailure.Value.Trim())
    }

    Add-PowerButtonTimingMarkerCheck $checks $Content

    return $checks
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


function Test-MonotonicSeismicResponse {
    param(
        [object]$Response,
        [int]$ExpectedRate
    )

    if ($null -eq $Response) {
        return [pscustomobject]@{ Pass = $false; Detail = "Empty /api/seismic response." }
    }

    $timestamps = @($Response.timestamps_ms)
    $count = [int]$Response.count
    $total = [uint64]$Response.total_frames
    $rate = [int]$Response.sample_rate_hz

    if ($rate -le 0) {
        return [pscustomobject]@{ Pass = $false; Detail = "sample_rate_hz=$rate is not valid." }
    }
    if ($ExpectedRate -gt 0 -and $rate -ne $ExpectedRate) {
        return [pscustomobject]@{ Pass = $false; Detail = "sample_rate_hz=$rate, expected $ExpectedRate." }
    }
    if ($Response.source -ne "AD7606") {
        return [pscustomobject]@{ Pass = $false; Detail = "source=$($Response.source), expected AD7606." }
    }
    if ($count -le 0 -or $timestamps.Count -ne $count -or $total -le 0) {
        return [pscustomobject]@{ Pass = $false; Detail = "count=$count timestamps=$($timestamps.Count) total_frames=$total." }
    }
    $channels = @($Response.channels)
    if ($channels.Count -ne 4) {
        return [pscustomobject]@{ Pass = $false; Detail = "channels=$($channels.Count), expected 4." }
    }
    $channelIds = @($channels | ForEach-Object { [int]$_.id })
    if ($channelIds.Count -ne 4 -or ($channelIds -join ",") -ne "1,2,3,4") {
        return [pscustomobject]@{ Pass = $false; Detail = "channel IDs=[$($channelIds -join ',')], expected [1,2,3,4]." }
    }
    foreach ($channel in $channels) {
        if (@($channel.samples_uv).Count -ne $count) {
            return [pscustomobject]@{ Pass = $false; Detail = "A channel contains $(@($channel.samples_uv).Count) samples, expected $count." }
        }
    }
    for ($i = 1; $i -lt $timestamps.Count; ++$i) {
        if ([int64]$timestamps[$i] -le [int64]$timestamps[$i - 1]) {
            return [pscustomobject]@{ Pass = $false; Detail = "timestamps_ms are not strictly increasing at index $i." }
        }
    }
    if ($timestamps.Count -gt 1) {
        $expectedIntervalMs = 1000.0 / [double]$rate
        $intervalToleranceMs = [math]::Max(1.0, $expectedIntervalMs * 0.10)
        for ($i = 1; $i -lt $timestamps.Count; ++$i) {
            $intervalMs = [double]$timestamps[$i] - [double]$timestamps[$i - 1]
            if ([math]::Abs($intervalMs - $expectedIntervalMs) -gt $intervalToleranceMs) {
                return [pscustomobject]@{
                    Pass = $false
                    Detail = "timestamp interval at index $i=${intervalMs}ms, expected ${expectedIntervalMs}ms +/- ${intervalToleranceMs}ms."
                }
            }
        }
        $averageIntervalMs =
            ([double]$timestamps[$timestamps.Count - 1] - [double]$timestamps[0]) /
            [double]($timestamps.Count - 1)
        if ([math]::Abs($averageIntervalMs - $expectedIntervalMs) -gt $intervalToleranceMs) {
            return [pscustomobject]@{
                Pass = $false
                Detail = "average timestamp interval=${averageIntervalMs}ms, expected ${expectedIntervalMs}ms +/- ${intervalToleranceMs}ms."
            }
        }
    }
    return [pscustomobject]@{
        Pass = $true
        Detail = "source=$($Response.source) rate=${rate}Hz count=$count total_frames=$total; timestamps monotonic and rate interval valid."
    }
}


function Test-RecordingSessionResponse {
    param(
        [object]$Response,
        [string]$ExpectedObjectId,
        [string]$ExpectedSessionId,
        [int]$ExpectedRate,
        [int]$ExpectedChannels = 4
    )

    if ($null -eq $Response -or $null -eq $Response.recording_sessions) {
        return [pscustomobject]@{ Pass = $false; Detail = "Missing recording_sessions in /api/status response." }
    }

    $collection = $Response.recording_sessions
    if ($collection.available -ne $true) {
        return [pscustomobject]@{ Pass = $false; Detail = "recording_sessions.available is not true." }
    }
    if ($collection.object_id -ne $ExpectedObjectId) {
        return [pscustomobject]@{ Pass = $false; Detail = "recording_sessions.object_id=$($collection.object_id), expected $ExpectedObjectId." }
    }

    $sessions = @($collection.sessions)
    $valid = @(
        $sessions | Where-Object {
            $_.session_id -eq $ExpectedSessionId -and
            $_.complete -eq $true -and
            [int]$_.sample_rate_hz -eq $ExpectedRate -and
            [int]$_.channel_count -eq $ExpectedChannels -and
            $null -ne $_.PSObject.Properties["samples_written"] -and
            $null -ne $_.PSObject.Properties["dropped_frames"] -and
            [uint64]$_.samples_written -gt 0 -and
            [uint64]$_.dropped_frames -eq 0
        }
    )
    if ($valid.Count -eq 0) {
        return [pscustomobject]@{ Pass = $false; Detail = "Session $ExpectedSessionId was not listed as complete with ${ExpectedRate}Hz/${ExpectedChannels} channels, samples_written>0 and dropped_frames=0." }
    }

    $session = $valid[0]
    return [pscustomobject]@{
        Pass = $true
        Detail = "Exact session $($session.session_id) listed at $($session.sample_rate_hz)Hz/$($session.channel_count) channels with samples_written=$($session.samples_written), dropped_frames=$($session.dropped_frames)."
    }
}


function Test-RuntimeResponsiveness {
    param(
        [object]$Before,
        [object]$After,
        [int]$MaxHeartbeatAgeMs = 1000,
        [switch]$RequireRecorderProgress
    )

    if (
        $null -eq $Before -or
        $null -eq $After -or
        $null -eq $Before.runtime -or
        $null -eq $After.runtime -or
        $null -eq $Before.runtime.tasks -or
        $null -eq $After.runtime.tasks
    ) {
        return [pscustomobject]@{
            Pass = $false
            Detail = "Missing runtime.tasks or runtime.now_ms in /api/status response."
        }
    }

    $beforeNowProperty = $Before.runtime.PSObject.Properties["now_ms"]
    $afterNowProperty = $After.runtime.PSObject.Properties["now_ms"]
    if (
        $null -eq $beforeNowProperty -or
        $null -eq $afterNowProperty -or
        $beforeNowProperty.Value -isnot [ValueType] -or
        $afterNowProperty.Value -isnot [ValueType]
    ) {
        return [pscustomobject]@{
            Pass = $false
            Detail = "Missing numeric runtime.now_ms in /api/status response."
        }
    }

    $beforeNowMs = [uint64]$Before.runtime.now_ms
    $afterNowMs = [uint64]$After.runtime.now_ms
    $taskNames = @("acquisition", "recorder", "ui")
    $stalled = [System.Collections.Generic.List[string]]::new()
    $unobserved = [System.Collections.Generic.List[string]]::new()
    $progressStalled = [System.Collections.Generic.List[string]]::new()
    $stale = [System.Collections.Generic.List[string]]::new()
    $future = [System.Collections.Generic.List[string]]::new()

    if ($afterNowMs -lt $beforeNowMs) {
        $stale.Add(("runtime.now_ms moved backwards from {0} to {1}" -f $beforeNowMs, $afterNowMs))
    }

    foreach ($name in $taskNames) {
        $beforeTask = $Before.runtime.tasks.$name
        $afterTask = $After.runtime.tasks.$name
        if ($null -eq $beforeTask -or $null -eq $afterTask) {
            $unobserved.Add($name)
            continue
        }
        if ($afterTask.observed -ne $true) {
            $unobserved.Add($name)
            continue
        }
        $heartbeatProperty = $afterTask.PSObject.Properties["last_heartbeat_ms"]
        if ($null -eq $heartbeatProperty -or $heartbeatProperty.Value -isnot [ValueType]) {
            $unobserved.Add("$name (missing last_heartbeat_ms)")
            continue
        }
        $lastHeartbeatMs = [uint64]$afterTask.last_heartbeat_ms
        if ($lastHeartbeatMs -gt $afterNowMs) {
            $future.Add($name)
        } elseif (($afterNowMs - $lastHeartbeatMs) -gt [uint64]$MaxHeartbeatAgeMs) {
            $stale.Add(("{0} ({1}ms > {2}ms)" -f $name, ($afterNowMs - $lastHeartbeatMs), $MaxHeartbeatAgeMs))
        }
        if ([uint64]$afterTask.heartbeat -le [uint64]$beforeTask.heartbeat) {
            $stalled.Add($name)
        }
        if (
            ($name -eq "acquisition" -or $name -eq "recorder") -and
            $RequireRecorderProgress -and
            [uint64]$afterTask.progress -le [uint64]$beforeTask.progress
        ) {
            $progressStalled.Add($name)
        }
    }

    if ($unobserved.Count -gt 0 -or $stalled.Count -gt 0 -or $progressStalled.Count -gt 0 -or $stale.Count -gt 0 -or $future.Count -gt 0) {
        $parts = [System.Collections.Generic.List[string]]::new()
        if ($unobserved.Count -gt 0) { $parts.Add("unobserved: $($unobserved -join ', ')") }
        if ($stalled.Count -gt 0) { $parts.Add("heartbeat stalled: $($stalled -join ', ')") }
        if ($progressStalled.Count -gt 0) { $parts.Add("progress stalled: $($progressStalled -join ', ')") }
        if ($stale.Count -gt 0) { $parts.Add("heartbeat too old: $($stale -join ', ')") }
        if ($future.Count -gt 0) { $parts.Add("heartbeat timestamp is in the future: $($future -join ', ')") }
        return [pscustomobject]@{
            Pass = $false
            Detail = $parts -join "; "
        }
    }

    return [pscustomobject]@{
        Pass = $true
        Detail = "All five task heartbeats advanced and are at most ${MaxHeartbeatAgeMs}ms old at runtime.now_ms=$afterNowMs; acquisition/recorder progress advanced when recording was required."
    }
}


function Invoke-HttpPipelineChecks {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$RootUrl,
        [int]$ExpectedRate,
        [int]$PollMs,
        [int]$MaxHeartbeatAgeMs,
        [switch]$DoRecord,
        [string]$RecordObjectId
    )

    $base = $RootUrl.TrimEnd("/")
    $seismic = $null
    try {
        $seismic = Invoke-JsonRequest ("{0}/api/seismic?points=120" -f $base)
        $seismicCheck = Test-MonotonicSeismicResponse $seismic $ExpectedRate
        Add-Check $Checks "HTTP /api/seismic" $(if ($seismicCheck.Pass) { "PASS" } else { "FAIL" }) $seismicCheck.Detail
        if ($seismicCheck.Pass) {
            Start-Sleep -Milliseconds $PollMs
            $nextSeismic = Invoke-JsonRequest ("{0}/api/seismic?points=120" -f $base)
            if ([uint64]$nextSeismic.total_frames -gt [uint64]$seismic.total_frames) {
                Add-Check $Checks "HTTP acquisition advances" "PASS" ("total_frames advanced from {0} to {1} while polling." -f $seismic.total_frames, $nextSeismic.total_frames)
            } else {
                Add-Check $Checks "HTTP acquisition advances" "FAIL" ("total_frames did not advance from {0}." -f $seismic.total_frames)
            }
        } else {
            Add-Check $Checks "HTTP acquisition advances" "FAIL" "Skipped because the first /api/seismic response failed validation."
        }
    } catch {
        Add-Check $Checks "HTTP /api/seismic" "FAIL" $_.Exception.Message
        Add-Check $Checks "HTTP acquisition advances" "FAIL" "Second poll could not be completed."
    }

    $status = $null
    $statusHealthy = $false
    try {
        $status = Invoke-JsonRequest ("{0}/api/status" -f $base)
        $acquisition = $status.acquisition
        $counterFieldsPresent =
            $null -ne $acquisition -and
            $null -ne $acquisition.PSObject.Properties["read_errors"] -and
            $null -ne $acquisition.PSObject.Properties["consecutive_failures"] -and
            $acquisition.read_errors -is [ValueType] -and
            $acquisition.consecutive_failures -is [ValueType]
        if (
            $null -ne $acquisition -and
            $acquisition.running -eq $true -and
            $acquisition.hardware_ready -eq $true -and
            $acquisition.source -eq "AD7606" -and
            [int]$acquisition.sample_rate_hz -eq $ExpectedRate -and
            [int]$acquisition.last_error -eq 0 -and
            $counterFieldsPresent -and
            [uint64]$acquisition.read_errors -eq 0 -and
            [uint64]$acquisition.consecutive_failures -eq 0
        ) {
            $statusHealthy = $true
            Add-Check $Checks "HTTP /api/status acquisition" "PASS" ("AD7606 running at {0}Hz; hardware_ready=true; read_errors=0; consecutive_failures=0." -f $acquisition.sample_rate_hz)
        } else {
            Add-Check $Checks "HTTP /api/status acquisition" "FAIL" "Acquisition status is not healthy AD7606 at the expected rate with read_errors=0 and consecutive_failures=0."
        }
    } catch {
        Add-Check $Checks "HTTP /api/status acquisition" "FAIL" $_.Exception.Message
    }

    if ($statusHealthy) {
        try {
            Start-Sleep -Milliseconds $PollMs
            $nextStatus = Invoke-JsonRequest ("{0}/api/status" -f $base)
            $runtimeCheck = Test-RuntimeResponsiveness $status $nextStatus $MaxHeartbeatAgeMs
            Add-Check $Checks "HTTP task responsiveness" $(if ($runtimeCheck.Pass) { "PASS" } else { "FAIL" }) $runtimeCheck.Detail
        } catch {
            Add-Check $Checks "HTTP task responsiveness" "FAIL" $_.Exception.Message
        }
    } else {
        Add-Check $Checks "HTTP task responsiveness" "FAIL" "Skipped because the first /api/status acquisition response was not healthy."
    }

    if (-not $DoRecord) {
        Add-Check $Checks "HTTP recording start/stop" "SKIPPED" "Use -Record -ObjectId <existing-id> to exercise recording explicitly."
        return
    }

    if ([string]::IsNullOrWhiteSpace($RecordObjectId)) {
        Add-Check $Checks "HTTP recording start/stop" "BLOCKED" "-Record requires an explicit -ObjectId."
        return
    }

    $started = $false
    try {
        $body = @{ action = "recording_start"; object_id = $RecordObjectId } | ConvertTo-Json -Compress
        $start = Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $body
        if ($start.ok -ne $true) {
            throw ("recording_start rejected: {0}" -f ($start.error | Out-String).Trim())
        }
        $started = $true

        $deadline = (Get-Date).AddSeconds(20)
        do {
            Start-Sleep -Milliseconds $PollMs
            $status = Invoke-JsonRequest ("{0}/api/status" -f $base)
            if ($status.recording.running -eq $true) {
                break
            }
        } while ((Get-Date) -lt $deadline)
        if ($status.recording.running -ne $true) {
            throw "recording_start was accepted but recording.running did not become true."
        }

        $startedSessionId = [string]$status.recording.session_id
        if ([string]::IsNullOrWhiteSpace($startedSessionId)) {
            throw "recording_start became active but /api/status did not report a session_id."
        }

        $recordingBaseline = $status
        # The recorder intentionally batches roughly 500 ms of frames before
        # each SD write; leave one full batch window between progress samples.
        $recordingProgressPollMs = [math]::Max($PollMs, 500)
        Start-Sleep -Milliseconds $recordingProgressPollMs
        $recordingProgress = Invoke-JsonRequest ("{0}/api/status" -f $base)
        $runtimeRecordingCheck = Test-RuntimeResponsiveness $recordingBaseline $recordingProgress $MaxHeartbeatAgeMs -RequireRecorderProgress
        Add-Check $Checks "HTTP recording task responsiveness" $(if ($runtimeRecordingCheck.Pass) { "PASS" } else { "FAIL" }) $runtimeRecordingCheck.Detail

        $stopBody = @{ action = "recording_stop" } | ConvertTo-Json -Compress
        $stop = Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $stopBody
        if ($stop.ok -ne $true) {
            throw ("recording_stop rejected: {0}" -f ($stop.error | Out-String).Trim())
        }

        $deadline = (Get-Date).AddSeconds(30)
        do {
            Start-Sleep -Milliseconds $PollMs
            $status = Invoke-JsonRequest ("{0}/api/status" -f $base)
            if ($status.recording.running -ne $true) {
                break
            }
        } while ((Get-Date) -lt $deadline)
        if ($status.recording.running -eq $true) {
            throw "recording_stop was accepted but recording.running stayed true."
        }
        if ([string]$status.recording.session_id -ne $startedSessionId) {
            throw "recording session_id changed from $startedSessionId to $($status.recording.session_id) after stop."
        }
        $recordingFieldsPresent =
            $null -ne $status.recording.PSObject.Properties["samples_written"] -and
            $null -ne $status.recording.PSObject.Properties["dropped_frames"]
        $samplesWritten = [uint64]$status.recording.samples_written
        $droppedFrames = [uint64]$status.recording.dropped_frames
        if (-not $recordingFieldsPresent) {
            Add-Check $Checks "SD recorder evidence" "FAIL" "recording_stop response did not expose samples_written and dropped_frames."
            Add-Check $Checks "HTTP recording start/stop" "FAIL" ("Completed for object_id={0}, session_id={1}, but recorder counters were missing." -f $RecordObjectId, $startedSessionId)
        } elseif ($samplesWritten -eq 0) {
            Add-Check $Checks "SD recorder evidence" "FAIL" "recording_stop completed but samples_written=0."
            Add-Check $Checks "HTTP recording start/stop" "FAIL" ("Completed for object_id={0}, but no persisted sample count was reported." -f $RecordObjectId)
        } elseif ($droppedFrames -ne 0) {
            Add-Check $Checks "SD recorder evidence" "FAIL" ("recording_stop completed with dropped_frames={0}; expected 0." -f $droppedFrames)
            Add-Check $Checks "HTTP recording start/stop" "FAIL" ("Completed for object_id={0}, session_id={1}, but dropped_frames={2}." -f $RecordObjectId, $startedSessionId, $droppedFrames)
        } else {
            Add-Check $Checks "SD recorder evidence" "PASS" ("Recorder reported samples_written={0}, dropped_frames=0 for object_id={1}, session_id={2}." -f $samplesWritten, $RecordObjectId, $startedSessionId)
            Add-Check $Checks "HTTP recording start/stop" "PASS" ("Completed for object_id={0}, session_id={1}; samples_written={2}, dropped_frames=0." -f $RecordObjectId, $startedSessionId, $samplesWritten)
        }

        try {
            $encodedObjectId = [uri]::EscapeDataString($RecordObjectId)
            $sessions = Invoke-JsonRequest ("{0}/api/status?sessions={1}" -f $base, $encodedObjectId)
            $sessionCheck = Test-RecordingSessionResponse $sessions $RecordObjectId $startedSessionId $ExpectedRate
            Add-Check $Checks "SD session metadata" $(if ($sessionCheck.Pass) { "PASS" } else { "FAIL" }) $sessionCheck.Detail
        } catch {
            Add-Check $Checks "SD session metadata" "FAIL" $_.Exception.Message
        }
    } catch {
        if ($started) {
            try {
                $stopBody = @{ action = "recording_stop" } | ConvertTo-Json -Compress
                [void](Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $stopBody)
            } catch {
                # Preserve the original failure in the report.
            }
        }
        Add-Check $Checks "HTTP recording start/stop" "FAIL" $_.Exception.Message
    }
}


$checks = [System.Collections.Generic.List[object]]::new()
$monitor = $null
$logFile = $null
$stderrFile = $null
$isReplay = -not [string]::IsNullOrWhiteSpace($LogPath)

try {
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

    if ($RequireFullPipeline) {
        $fullPrerequisitesValid = $true
        if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
            $fullPrerequisitesValid = $false
            Add-Check $checks "Full pipeline prerequisites" "BLOCKED" "-RequireFullPipeline requires -BaseUrl for /api checks."
        }
        if (-not $Record -or [string]::IsNullOrWhiteSpace($ObjectId)) {
            $fullPrerequisitesValid = $false
            Add-Check $checks "Full pipeline prerequisites" "BLOCKED" "-RequireFullPipeline requires -Record -ObjectId <existing-id> for SD evidence."
        }
        if (-not $fullPrerequisitesValid) {
            throw "Full pipeline prerequisites are incomplete; stopping before serial preflight, monitor, or flash."
        }
    }

    if ($isReplay) {
        $logFile = (Resolve-Path -LiteralPath $LogPath).Path
        Add-Check $checks "Live serial preflight" "SKIPPED" "Replay mode from -LogPath; no hardware access."
        Add-Check $checks "Production flash" "SKIPPED" "Replay mode from -LogPath; no flash access."
        Add-Check $checks "Pipeline log replay" "PASS" ("Parsing captured log: {0}" -f $logFile)
    } else {
        # A child powershell process receives its argument list as scalars;
        # pass the helper's supported comma form so both ports bind reliably.
        $requestedPorts = "{0},{1}" -f $FlashPort, $LogPort
        $preflightStamp = [guid]::NewGuid().ToString("N")
        $preflightStdout = Join-Path ([IO.Path]::GetTempPath()) ("geolocator-port-check-{0}.out" -f $preflightStamp)
        $preflightStderr = Join-Path ([IO.Path]::GetTempPath()) ("geolocator-port-check-{0}.err" -f $preflightStamp)
        $preflightErrorAction = $ErrorActionPreference
        try {
            # Native powershell writes a child script's terminating error to
            # stderr.  Keep that diagnostic in the temporary file so the
            # parent can reliably read LASTEXITCODE and print one final row.
            $ErrorActionPreference = "Continue"
            & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $checkPorts -Ports $requestedPorts 1> $preflightStdout 2> $preflightStderr
            $preflightExitCode = $LASTEXITCODE
            $preflightOutput = @(
                (Get-Content -LiteralPath $preflightStdout -ErrorAction SilentlyContinue),
                (Get-Content -LiteralPath $preflightStderr -ErrorAction SilentlyContinue)
            )
        } finally {
            $ErrorActionPreference = $preflightErrorAction
            Remove-Item -LiteralPath $preflightStdout, $preflightStderr -Force -ErrorAction SilentlyContinue
        }
        if ($preflightExitCode -ne 0) {
            $preflightDetail = $preflightOutput |
                Where-Object { "$_" -match "Required live serial port\(s\) missing:" } |
                Select-Object -First 1
            if ([string]::IsNullOrWhiteSpace($preflightDetail)) {
                $preflightDetail = ($preflightOutput -join " ")
            }
            $preflightDetail = [regex]::Replace($preflightDetail, "[^\x20-\x7E]", " ")
            $preflightDetail = [regex]::Replace($preflightDetail, "\s+", " ").Trim()
            $summaryMatch = [regex]::Match(
                $preflightDetail,
                "Required live serial port\(s\) missing:\s*[^.]*\.\s*Live ports:\s*[^.]*\."
            )
            if ($summaryMatch.Success) {
                $preflightDetail = $summaryMatch.Value.Trim()
            }
            if ([string]::IsNullOrWhiteSpace($preflightDetail)) {
                $preflightDetail = "No diagnostic output."
            }
            throw "Live COM4/COM6 preflight failed with exit code ${preflightExitCode}: $preflightDetail"
        }
        Add-Check $checks "Live serial preflight" "PASS" ("Live ports available: {0}, {1}." -f $FlashPort, $LogPort)

        $hilDir = Join-Path $repo "build\hil"
        New-Item -ItemType Directory -Path $hilDir -Force | Out-Null
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $logFile = Join-Path $hilDir "pipeline-production-$stamp.log"
        $stderrFile = Join-Path $hilDir "pipeline-production-$stamp.err.log"
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
        Write-Host ("Starting production monitor on {0}; output: {1}" -f $LogPort, $logFile)
        $monitor = Start-Process -FilePath $powershell -ArgumentList $monitorArgs -WorkingDirectory $project -RedirectStandardOutput $logFile -RedirectStandardError $stderrFile -PassThru -WindowStyle Hidden
        Start-Sleep -Seconds 2
        if ($monitor.HasExited) {
            throw "COM6 monitor exited before pipeline execution. See $stderrFile."
        }

        if ($SkipFlash) {
            Add-Check $checks "Production flash" "SKIPPED" "-SkipFlash supplied; existing production firmware is being observed."
        } else {
            & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $runIdf -Action flash -ProjectPath $ProjectPath -BuildDir $BuildDir -Port $FlashPort
            if ($LASTEXITCODE -ne 0) {
                throw "Production flash failed with exit code $LASTEXITCODE."
            }
            Add-Check $checks "Production flash" "PASS" ("Flashed through {0}." -f $FlashPort)
        }
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $parsedChecks = $null
    do {
        $content = Read-PipelineLog $logFile
        if (-not $isReplay -and $stderrFile) {
            $content = $content + "`n" + (Read-PipelineLog $stderrFile)
        }
        $parsedChecks = Test-PipelineLog $content $ExpectedSampleRateHz
        $required = @(
            "AD7606 GPIO configuration",
            "AD7606 SPI3 initialization",
            "AD7606 self-test",
            "AD7606 acquisition started",
            "AD7606 channel diagnostics"
        )
        if ($ConfirmPowerButtonTiming) {
            $required += "Power button timing marker"
        }
        $missing = @($parsedChecks | Where-Object { $_.Name -in $required -and $_.Status -ne "PASS" })
        if ($missing.Count -eq 0) {
            break
        }
        if ($isReplay) {
            # A captured log is immutable: one parse is enough to report its
            # missing markers and must never wait for the live timeout.
            break
        }
        if (-not $isReplay -and $null -ne $monitor -and $monitor.HasExited) {
            break
        }
        if ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds $PollIntervalMs
        }
    } while ((Get-Date) -lt $deadline)

    foreach ($parsed in $parsedChecks) {
        Add-Check $checks $parsed.Name $parsed.Status $parsed.Detail
    }

    Add-PhysicalObservationCheck $checks `
        "Physical TFT rendering" `
        ([bool]$ConfirmTftRendering) `
        ([bool]$isReplay) `
        ([bool]$RequireFullPipeline) `
        "Inspect the live Measurements screen and confirm the TFT is rendering the current acquisition state." `
        "Serial replay cannot prove TFT pixels." `
        "ConfirmTftRendering"
    Add-PhysicalObservationCheck $checks `
        "Physical keypad responsiveness" `
        ([bool]$ConfirmKeypadResponsive) `
        ([bool]$isReplay) `
        ([bool]$RequireFullPipeline) `
        "Press the keypad while acquisition/recording is active and confirm navigation remains responsive." `
        "Serial replay cannot prove physical keypad responsiveness." `
        "ConfirmKeypadResponsive"
    Add-PhysicalObservationCheck $checks `
        "Physical power-button responsiveness" `
        ([bool]$ConfirmPowerButtonResponsive) `
        ([bool]$isReplay) `
        ([bool]$RequireFullPipeline) `
        "Press the power button during acquisition and confirm ordered shutdown completes." `
        "Serial replay cannot prove physical power-button responsiveness." `
        "ConfirmPowerButtonResponsive"
    Add-PowerButtonTimingCheck $checks `
        ([bool]$ConfirmPowerButtonTiming) `
        ([bool]$isReplay) `
        ([bool]$RequireFullPipeline) `
        $PowerButtonTimingMs `
        $PowerButtonTimingTargetMs `
        $PowerButtonTimingToleranceMs

    if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
        if ($Record) {
            Add-Check $checks "Recording argument" "BLOCKED" "-Record requires -BaseUrl and an explicit -ObjectId."
        }
        Add-Check $checks "HTTP pipeline checks" "SKIPPED" "Pass -BaseUrl to poll /api/seismic and /api/status."
    } else {
        $reportedRate = Get-PipelineReportedSampleRate $content
        $httpExpectedRate = if ($ExpectedSampleRateHz -gt 0) {
            $ExpectedSampleRateHz
        } else {
            $reportedRate
        }
        Invoke-HttpPipelineChecks $checks $BaseUrl $httpExpectedRate $PollIntervalMs $MaxTaskHeartbeatAgeMs $Record $ObjectId
    }
} catch {
    Add-Check $checks "Runner execution" $(if ($isReplay) { "FAIL" } else { "BLOCKED" }) $_.Exception.Message
} finally {
    if ($null -ne $monitor -and -not $monitor.HasExited) {
        Stop-Process -Id $monitor.Id -Force -ErrorAction SilentlyContinue
        $monitor.WaitForExit()
    }
}

$failed = @($checks | Where-Object { $_.Status -eq "FAIL" })
$blocked = @($checks | Where-Object { $_.Status -eq "BLOCKED" })
$result = if ($failed.Count -gt 0) {
    "FAIL"
} elseif ($blocked.Count -gt 0) {
    "BLOCKED"
} else {
    "PASS"
}

Write-Host "===== Production pipeline HIL report ====="
Write-Host ("{0,-38} {1,-9} {2}" -f "CHECK", "STATUS", "DETAIL")
Write-Host ("{0,-38} {1,-9} {2}" -f "-----", "------", "------")
foreach ($check in $checks) {
    Write-Host ("{0,-38} {1,-9} {2}" -f $check.Name, $check.Status, $check.Detail)
}
if ($isReplay) {
    Write-Host ("LOG_REPLAY: {0}" -f $logFile)
}
Write-Host "LIMITATIONS: TFT pixels, physical keypad/power-button responsiveness, and raw GPIO21 HIGH -> shutdown-latch timing require live hardware observation; use the explicit -ConfirmTftRendering, -ConfirmKeypadResponsive, -ConfirmPowerButtonResponsive and -ConfirmPowerButtonTiming switches. The timing switch also requires measured -PowerButtonTimingMs, -PowerButtonTimingTargetMs and -PowerButtonTimingToleranceMs values."
Write-Host ("PIPELINE_HIL_RESULT: {0}" -f $result)

if ($result -eq "PASS") {
    exit 0
}
exit 1
