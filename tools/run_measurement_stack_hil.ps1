[CmdletBinding()]
param(
    [string]$FlashPort = "COM4",
    [string]$LogPort = "COM6",
    [string]$ProjectPath = ".",
    [string]$BuildDir = "build",
    [string]$BaseUrl,
    [string]$ObjectId,
    [string]$ExpectedObjectNamePrefix = "HIL_STACK_TEST",
    [string]$LogPath,
    [switch]$Flash,

    [ValidateRange(20, 200)]
    [int]$Cycles = 20,

    [ValidateRange(100, 10000)]
    [int]$RecordIntervalMs = 500,

    [ValidateRange(50, 5000)]
    [int]$PollIntervalMs = 250,

    [ValidateRange(5, 120)]
    [int]$TimeoutSeconds = 30,

    [ValidateRange(1, 4096)]
    [int]$MinimumMeasurementServiceHwmBytes = 1024
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


function Read-TargetLog {
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


function Count-Pattern {
    param(
        [string]$Content,
        [string]$Pattern
    )

    return @([regex]::Matches($Content, $Pattern)).Count
}


function Get-MeasurementHwmStats {
    param([string]$Content)

    $matches = [regex]::Matches(
        $Content,
        "(?im)STACK_HWM\s+task=measurement_service\s+stage=(?<stage>[A-Za-z0-9_-]+)\s+free=(?<free>\d+)(?:\s+margin=(?<margin>\d+))?"
    )
    $stageCounts = @{}
    $minimum = $null
    foreach ($match in $matches) {
        $stage = $match.Groups["stage"].Value
        $free = [int]$match.Groups["free"].Value
        if (-not $stageCounts.ContainsKey($stage)) {
            $stageCounts[$stage] = 0
        }
        $stageCounts[$stage]++
        if ($null -eq $minimum -or $free -lt $minimum) {
            $minimum = $free
        }
    }

    return [pscustomobject]@{
        Count = $matches.Count
        MinimumFree = if ($null -eq $minimum) { -1 } else { $minimum }
        StageCounts = $stageCounts
    }
}


function Wait-ForLogCount {
    param(
        [string]$Path,
        [string]$Pattern,
        [int]$Minimum,
        [int]$TimeoutSeconds,
        [int]$PollMs
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $content = Read-TargetLog $Path
        if ((Count-Pattern $content $Pattern) -ge $Minimum) {
            return $true
        }
        if ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds $PollMs
        }
    } while ((Get-Date) -lt $deadline)
    return $false
}


function Invoke-JsonRequest {
    param(
        [string]$Uri,
        [ValidateSet("GET", "POST")]
        [string]$Method = "GET",
        [string]$Body,
        [int]$TimeoutSec = 10
    )

    $request = @{
        Uri = $Uri
        Method = $Method
        TimeoutSec = $TimeoutSec
        ErrorAction = "Stop"
    }
    if (-not [string]::IsNullOrWhiteSpace($Body)) {
        $request.Body = $Body
        $request.ContentType = "application/json"
    }
    return Invoke-RestMethod @request
}


function Test-TargetLog {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Content,
        [int]$RequiredCycles,
        [int]$MinimumHwmBytes
    )

    if ([string]::IsNullOrWhiteSpace($Content)) {
        Add-Check $Checks "Target serial log" "FAIL" "The log is empty or could not be read."
        return
    }

    $stats = Get-MeasurementHwmStats $Content
    $requiredStages = @(
        @("STACK_HWM before-start", "before-start"),
        @("STACK_HWM after-recorder-start", "after-recorder-start"),
        @("STACK_HWM after-stop", "after-stop")
    )
    foreach ($stage in $requiredStages) {
        $count = if ($stats.StageCounts.ContainsKey($stage[1])) { $stats.StageCounts[$stage[1]] } else { 0 }
        Add-Check $Checks $stage[0] $(if ($count -ge $RequiredCycles) { "PASS" } else { "FAIL" }) ("Observed {0}; required at least {1}." -f $count, $RequiredCycles)
    }

    Add-Check $Checks "STACK_HWM safety margin" `
        $(if ($stats.Count -gt 0 -and $stats.MinimumFree -ge $MinimumHwmBytes) { "PASS" } else { "FAIL" }) `
        ("minimum measurement_service free={0} bytes; required >= {1} bytes across {2} samples." -f $stats.MinimumFree, $MinimumHwmBytes, $stats.Count)

    $reviewCount = Count-Pattern $Content "(?im)MEASUREMENT_SERVICE:\s+Recording stopped state=REVIEW"
    Add-Check $Checks "Measurement state REVIEW" `
        $(if ($reviewCount -ge $RequiredCycles) { "PASS" } else { "FAIL" }) `
        ("Observed {0} REVIEW completions; required at least {1}." -f $reviewCount, $RequiredCycles)

    $panic = [regex]::Match(
        $Content,
        "(?im)(stack overflow|guru meditation|abort\(\)|corrupt heap|stack smashing|panic'ed|watchdog|task watchdog|\bWDT\b|brownout detector was triggered)"
    )
    Add-Check $Checks "Panic/watchdog/brownout detection" `
        $(if (-not $panic.Success) { "PASS" } else { "FAIL" }) `
        $(if (-not $panic.Success) { "No panic, stack overflow, watchdog, or brownout marker observed." } else { "Failure marker: $($panic.Value.Trim())." })

    $resetCount = Count-Pattern $Content "(?im)^\s*rst:\s*0x[0-9a-f]+"
    Add-Check $Checks "Unexpected reboot detection" `
        $(if ($resetCount -le 1) { "PASS" } else { "FAIL" }) `
        ("Observed {0} boot reset markers; expected at most one initial boot." -f $resetCount)

    $resourceFailure = [regex]::Match(
        $Content,
        "(?im)(failed to close uncommitted session|failed to remove uncommitted session|timed out stopping recorder|storage is not ready|failed to flush|failed to write samples|lvgl.*(timeout|deadlock|stuck|failed))"
    )
    Add-Check $Checks "Storage/session/LVGL cleanup" `
        $(if (-not $resourceFailure.Success) { "PASS" } else { "FAIL" }) `
        $(if (-not $resourceFailure.Success) { "No session-handle, storage-lock, or LVGL-lock failure marker observed." } else { "Failure marker: $($resourceFailure.Value.Trim())." })
}


function Wait-RecordingStart {
    param(
        [string]$Base,
        [string]$ExpectedObjectId,
        [int]$TimeoutSec,
        [int]$PollMs
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $status = Invoke-JsonRequest ("{0}/api/status" -f $Base)
        $recording = $status.recording
        if (
            $null -ne $recording -and
            $recording.running -eq $true -and
            $recording.state -eq "RECORDING" -and
            $recording.object_id -eq $ExpectedObjectId -and
            -not [string]::IsNullOrWhiteSpace([string]$recording.session_id)
        ) {
            return [string]$recording.session_id
        }
        if ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds $PollMs
        }
    } while ((Get-Date) -lt $deadline)
    throw "recording_start did not reach RECORDING for $ExpectedObjectId within ${TimeoutSec}s."
}


function Assert-MeasurementStartWorkflowReady {
    param(
        [string]$Base,
        [int]$RequestTimeoutSec,
        [string]$Action
    )

    $status = Invoke-JsonRequest ("{0}/api/status" -f $Base) "GET" $null $RequestTimeoutSec
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
        $status = Invoke-JsonRequest ("{0}/api/status" -f $Base) "GET" $null $TimeoutSec
        $workflow = $status.workflow
        $step = [string]$workflow.step
        if ($step -eq "RECORD_READY") { return }
        if ($step -eq "STOPPED") {
            $body = @{
                action = "workflow_command"
                command = "RECORD_AGAIN"
                generation = $workflow.generation
            } | ConvertTo-Json -Compress
            $result = Invoke-JsonRequest ("{0}/api/control" -f $Base) "POST" $body $TimeoutSec
            if ($result.ok -eq $true) { return }
            if ($result.reason -ne "stale") {
                throw "workflow RECORD_AGAIN rejected: $($result.reason)"
            }
        } elseif ($step -ne "RECORDING") {
            throw "workflow RECORD_AGAIN requires STOPPED or RECORD_READY; current step is '$step'."
        }
        if ((Get-Date) -lt $deadline) { Start-Sleep -Milliseconds $PollMs }
    } while ((Get-Date) -lt $deadline)

    throw "workflow did not become STOPPED or RECORD_READY within ${TimeoutSec}s after recording stop."
}


function Wait-RecordingStop {
    param(
        [string]$Base,
        [string]$ExpectedObjectId,
        [string]$ExpectedSessionId,
        [int]$ExpectedSampleRate,
        [int]$TimeoutSec,
        [int]$PollMs
    )

    $encodedObjectId = [uri]::EscapeDataString($ExpectedObjectId)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $status = Invoke-JsonRequest ("{0}/api/status?sessions={1}" -f $Base, $encodedObjectId)
        $recording = $status.recording
        $sessions = @($status.recording_sessions.sessions)
        $valid = @(
            $sessions | Where-Object {
                $_.session_id -eq $ExpectedSessionId -and
                $_.complete -eq $true -and
                [int]$_.sample_rate_hz -eq $ExpectedSampleRate -and
                [int]$_.channel_count -eq 4 -and
                [uint64]$_.samples_written -gt 0 -and
                [uint64]$_.dropped_frames -eq 0
            }
        )
        if (
            $null -ne $recording -and
            $recording.running -ne $true -and
            $recording.state -eq "IDLE" -and
            [int]$recording.last_error -eq 0 -and
            $valid.Count -gt 0
        ) {
            return $status
        }
        if ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds $PollMs
        }
    } while ((Get-Date) -lt $deadline)
    throw "recording_stop did not reach IDLE with a complete session $ExpectedSessionId within ${TimeoutSec}s."
}


function Start-SerialMonitor {
    param(
        [string]$Path,
        [string]$ErrorPath,
        [string]$Project,
        [string]$Build,
        [string]$Port
    )

    $runIdf = Join-Path $PSScriptRoot "run_idf.ps1"
    $arguments = @(
        "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"{0}"' -f $runIdf),
        "-Action", "monitor", "-ProjectPath", $Project,
        "-BuildDir", $Build, "-Port", $Port
    )
    return Start-Process `
        -FilePath "powershell.exe" `
        -ArgumentList $arguments `
        -WorkingDirectory (Resolve-RepoPath $Project) `
        -RedirectStandardOutput $Path `
        -RedirectStandardError $ErrorPath `
        -PassThru `
        -WindowStyle Hidden
}


$repo = Resolve-RepoPath "."
$checks = [System.Collections.Generic.List[object]]::new()
$monitor = $null
$logFile = $null
$stderrFile = $null
$base = if ([string]::IsNullOrWhiteSpace($BaseUrl)) { "" } else { $BaseUrl.TrimEnd("/") }
$previousActiveId = $null
$isReplay = -not [string]::IsNullOrWhiteSpace($LogPath)

try {
    if ($isReplay) {
        $logFile = (Resolve-Path -LiteralPath $LogPath).Path
        Test-TargetLog $checks (Read-TargetLog $logFile) $Cycles $MinimumMeasurementServiceHwmBytes
        Add-Check $checks "Live 20-cycle execution" "BLOCKED" "-LogPath can validate serial markers only; it cannot prove live START/STOP, session cleanup, or target liveness."
        Add-Check $checks "User-data safety" "PASS" "Replay mode performs no HTTP mutation, erase, delete, or flash."
    } else {
        if ([string]::IsNullOrWhiteSpace($base) -or [string]::IsNullOrWhiteSpace($ObjectId)) {
            throw "Live mode requires -BaseUrl and -ObjectId for a pre-created dedicated HIL object."
        }

        $checkPorts = Join-Path $repo "tools/check_serial_ports.ps1"
        $requestedPorts = if ($Flash) { "{0},{1}" -f $FlashPort, $LogPort } else { $LogPort }
        & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $checkPorts -Ports $requestedPorts
        if ($LASTEXITCODE -ne 0) {
            throw "Required HIL serial port preflight failed for $requestedPorts."
        }
        Add-Check $checks "Live serial preflight" "PASS" ("Live port(s) available: {0}." -f $requestedPorts)

        $hilDir = Join-Path $repo "build\hil"
        New-Item -ItemType Directory -Path $hilDir -Force | Out-Null
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $logFile = Join-Path $hilDir ("measurement-stack-{0}.log" -f $stamp)
        $stderrFile = Join-Path $hilDir ("measurement-stack-{0}.err.log" -f $stamp)
        $monitor = Start-SerialMonitor $logFile $stderrFile $ProjectPath $BuildDir $LogPort
        Start-Sleep -Seconds 2
        if ($monitor.HasExited) {
            throw "The target monitor exited before the HIL run. See $stderrFile."
        }

        if ($Flash) {
            $runIdf = Join-Path $repo "tools/run_idf.ps1"
            & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $runIdf `
                -Action flash -ProjectPath $ProjectPath -BuildDir $BuildDir -Port $FlashPort
            if ($LASTEXITCODE -ne 0) {
                throw "Explicit HIL flash failed with exit code $LASTEXITCODE."
            }
            Add-Check $checks "Production flash" "PASS" ("Explicit -Flash used through {0}." -f $FlashPort)
        } else {
            Add-Check $checks "Production flash" "SKIPPED" "No flash requested; observing the already-installed instrumented firmware."
        }

        if (-not (Wait-ForLogCount $logFile "(?im)STACK_HWM\s+task=measurement_service\s+stage=idle\s+free=\d+" 1 $TimeoutSeconds $PollIntervalMs)) {
            throw "Instrumented measurement_service idle HWM marker was not observed after boot."
        }
        Add-Check $checks "Instrumented target boot" "PASS" "measurement_service emitted an idle STACK_HWM marker."

        $objectsStatus = Invoke-JsonRequest ("{0}/api/status?objects=1" -f $base)
        if ($objectsStatus.object_store.available -ne $true) {
            throw "Object store is not available."
        }
        $target = @($objectsStatus.object_store.objects | Where-Object { $_.id -eq $ObjectId }) | Select-Object -First 1
        if ($null -eq $target) {
            throw "Dedicated HIL object $ObjectId does not exist; refusing to touch user objects."
        }
        if ([string]$target.name -notlike ("{0}*" -f $ExpectedObjectNamePrefix)) {
            throw "Object $ObjectId is named '$($target.name)'; expected a dedicated name beginning with '$ExpectedObjectNamePrefix'."
        }
        $previousActiveId = [string]$objectsStatus.object_store.active_id

        $activateBody = @{ action = "activate_object"; object_id = $ObjectId } | ConvertTo-Json -Compress
        $activate = Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $activateBody
        if ($activate.ok -ne $true) {
            throw "Dedicated HIL object activation was rejected: $($activate.error)"
        }
        Add-Check $checks "Dedicated test object" "PASS" ("Using pre-created $ObjectId named '$($target.name)'; no user object is deleted or erased.")

        $initialStatus = Invoke-JsonRequest ("{0}/api/status?sessions={1}" -f $base, [uri]::EscapeDataString($ObjectId))
        if ($initialStatus.recording.running -eq $true) {
            throw "Target is already recording; refusing to interrupt it."
        }
        if ([int]$initialStatus.recording.last_error -ne 0) {
            throw "Target recorder starts in error state $($initialStatus.recording.last_error); refusing to mask it."
        }

        $beforeCycles = @{}
        $seenSessions = [System.Collections.Generic.HashSet[string]]::new()
        for ($cycle = 1; $cycle -le $Cycles; ++$cycle) {
            $contentBeforeStart = Read-TargetLog $logFile
            $beforeCycles.before = Count-Pattern $contentBeforeStart "(?im)STACK_HWM\s+task=measurement_service\s+stage=before-start\s+free=\d+"
            $beforeCycles.review = Count-Pattern $contentBeforeStart "(?im)MEASUREMENT_SERVICE:\s+Recording stopped state=REVIEW"

            Assert-MeasurementStartWorkflowReady $base $TimeoutSeconds "measurement_start"
            $startBody = @{ action = "measurement_start"; object_id = $ObjectId } | ConvertTo-Json -Compress
            $start = Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $startBody
            if ($start.ok -ne $true) {
                throw "cycle $cycle measurement_start rejected: $($start.error)"
            }

            $sessionId = Wait-RecordingStart $base $ObjectId $TimeoutSeconds $PollIntervalMs
            [void]$seenSessions.Add($sessionId)
            Start-Sleep -Milliseconds $RecordIntervalMs

            $stopBody = @{ action = "measurement_stop" } | ConvertTo-Json -Compress
            $stop = Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $stopBody
            if ($stop.ok -ne $true) {
                throw "cycle $cycle measurement_stop rejected: $($stop.error)"
            }

            [void](Wait-RecordingStop $base $ObjectId $sessionId 500 $TimeoutSeconds $PollIntervalMs)
            if (-not (Wait-ForLogCount $logFile "(?im)MEASUREMENT_SERVICE:\s+Recording stopped state=REVIEW" ($beforeCycles.review + 1) $TimeoutSeconds $PollIntervalMs)) {
                throw "cycle $cycle did not produce the measurement_service REVIEW marker."
            }
            if ($cycle -lt $Cycles) {
                Invoke-WorkflowRecordAgain $base $TimeoutSeconds $PollIntervalMs
            }
            $contentAfterStop = Read-TargetLog $logFile
            $afterStartCount = Count-Pattern $contentAfterStop "(?im)STACK_HWM\s+task=measurement_service\s+stage=after-recorder-start\s+free=\d+"
            $afterStopCount = Count-Pattern $contentAfterStop "(?im)STACK_HWM\s+task=measurement_service\s+stage=after-stop\s+free=\d+"
            if ($afterStartCount -lt $cycle -or $afterStopCount -lt $cycle) {
                throw "cycle $cycle did not produce both after-recorder-start and after-stop HWM markers."
            }
            Write-Host ("cycle {0}/{1}: session={2} complete" -f $cycle, $Cycles, $sessionId)
        }

        $finalStatus = Invoke-JsonRequest ("{0}/api/status?sessions={1}" -f $base, [uri]::EscapeDataString($ObjectId))
        if ($finalStatus.recording.running -eq $true -or $finalStatus.recording.state -ne "IDLE") {
            throw "Final recorder state is not IDLE after $Cycles cycles."
        }
        if ($seenSessions.Count -ne $Cycles) {
            throw "Expected $Cycles unique session IDs but observed $($seenSessions.Count)."
        }
        Add-Check $checks "Live 20-cycle execution" "PASS" ("Completed {0} non-destructive START/record/STOP cycles with unique complete sessions." -f $Cycles)
        Add-Check $checks "Session/file handle cleanup" "PASS" "Every cycle reached recorder IDLE with a complete session and the next START was accepted; no session delete/erase was issued."

        $finalContent = Read-TargetLog $logFile
        Test-TargetLog $checks $finalContent $Cycles $MinimumMeasurementServiceHwmBytes
    }
} catch {
    Add-Check $checks "Runner execution" $(if ($isReplay) { "FAIL" } else { "BLOCKED" }) $_.Exception.Message
} finally {
    if (-not $isReplay -and $null -ne $monitor -and -not $monitor.HasExited) {
        Stop-Process -Id $monitor.Id -Force -ErrorAction SilentlyContinue
        $monitor.WaitForExit()
    }

    if (-not $isReplay -and -not [string]::IsNullOrWhiteSpace($base) -and -not [string]::IsNullOrWhiteSpace($previousActiveId) -and $previousActiveId -ne $ObjectId) {
        try {
            $restoreBody = @{ action = "activate_object"; object_id = $previousActiveId } | ConvertTo-Json -Compress
            $restore = Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $restoreBody
            if ($restore.ok -eq $true) {
                Add-Check $checks "Active object restore" "PASS" ("Restored the previously active object $previousActiveId.")
            } else {
                Add-Check $checks "Active object restore" "FAIL" "The previous active object could not be restored: $($restore.error)"
            }
        } catch {
            Add-Check $checks "Active object restore" "FAIL" $_.Exception.Message
        }
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

Write-Host "===== Measurement service stack HIL report ====="
Write-Host ("{0,-42} {1,-9} {2}" -f "CHECK", "STATUS", "DETAIL")
Write-Host ("{0,-42} {1,-9} {2}" -f "-----", "------", "------")
foreach ($check in $checks) {
    Write-Host ("{0,-42} {1,-9} {2}" -f $check.Name, $check.Status, $check.Detail)
}
if ($logFile) {
    Write-Host ("LOG_PATH: {0}" -f $logFile)
}
Write-Host "SAFETY: this runner requires a pre-created object whose name begins with HIL_STACK_TEST; it never deletes sessions or erases the SD card."
Write-Host ("MEASUREMENT_STACK_HIL_RESULT: {0}" -f $result)

if ($result -eq "PASS") {
    exit 0
}
exit 1
