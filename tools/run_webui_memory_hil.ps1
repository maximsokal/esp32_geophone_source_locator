[CmdletBinding()]
param(
    [string]$LogPort = "COM6",
    [string]$ProjectPath = ".",
    [string]$BuildDir = "build",
    [string]$BaseUrl,
    [string]$ObjectId,
    [string]$ExpectedObjectNamePrefix = "HIL_MEMORY_TEST",
    [string]$LogPath,

    [ValidateRange(1, 20)]
    [int]$Cycles = 1,

    [ValidateRange(5, 120)]
    [int]$RecordSeconds = 30,

    [ValidateRange(100, 5000)]
    [int]$PollIntervalMs = 500,

    [ValidateRange(5, 60)]
    [int]$RequestTimeoutSeconds = 10,

    # This is a test-run bound for recorder finalization, not a firmware stall
    # timeout.  STOP is asynchronous and may legitimately outlive one request.
    [ValidateRange(5, 180)]
    [int]$StopTimeoutSeconds = 30,

    [ValidateRange(5, 120)]
    [int]$BootTimeoutSeconds = 30,

    [ValidateRange(1, 30)]
    [int]$AssetIntervalSeconds = 5,

    # The cold root request includes the first TLS/HTTP setup. Subsequent
    # assets must stay below the tighter LAN-service budget.
    [ValidateRange(100, 60000)]
    [int]$MaxAssetLatencyMs = 1000,

    [ValidateRange(100, 120000)]
    [int]$MaxColdTlsLatencyMs = 2000,

    [ValidateRange(1, 4096)]
    [int]$MinimumMeasurementServiceHwmBytes = 1024,

    # Provisional safety floors. Calibrate them from the first real UART run,
    # then keep the values explicit in the HIL invocation.
    [ValidateRange(0, 1048576)]
    [int]$MinimumInternalFreeBytes = 32768,

    [ValidateRange(0, 1048576)]
    [int]$MinimumInternalLargestFreeBlockBytes = 8192,

    [ValidateRange(0, 1048576)]
    [int]$MinimumDmaFreeBytes = 16384,

    [ValidateRange(0, 1048576)]
    [int]$MinimumDmaLargestFreeBlockBytes = 4096,

    [ValidateRange(0, 8388608)]
    [int]$MinimumPsramFreeBytes = 1048576
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


function Invoke-HttpRequest {
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
        UseBasicParsing = $true
        ErrorAction = "Stop"
    }
    if (-not [string]::IsNullOrWhiteSpace($Body)) {
        $request.Body = $Body
        $request.ContentType = "application/json"
    }
    return Invoke-WebRequest @request
}


function Invoke-JsonRequest {
    param(
        [string]$Uri,
        [ValidateSet("GET", "POST")]
        [string]$Method = "GET",
        [string]$Body,
        [int]$TimeoutSec = 10
    )

    $response = Invoke-HttpRequest $Uri $Method $Body $TimeoutSec
    if ($response.StatusCode -lt 200 -or $response.StatusCode -ge 300) {
        throw "HTTP $($response.StatusCode) from $Uri"
    }
    return ($response.Content | ConvertFrom-Json)
}


function Invoke-WebUiAsset {
    param(
        [string]$Base,
        [string]$Path,
        [int]$TimeoutSec,
        [int]$MaxLatencyMs
    )

    $watch = [Diagnostics.Stopwatch]::StartNew()
    $response = Invoke-HttpRequest ("{0}{1}" -f $Base, $Path) "GET" $null $TimeoutSec
    $watch.Stop()
    $elapsedMs = [int]$watch.ElapsedMilliseconds
    $bytes = if ($null -eq $response.Content) { 0 } else { $response.Content.Length }

    if ($response.StatusCode -ne 200) {
        throw "WebUI asset $Path returned HTTP $($response.StatusCode)."
    }

    Write-Host ("WEBUI_ASSET path={0} elapsed_ms={1} bytes={2}" -f $Path, $elapsedMs, $bytes)
    if ($elapsedMs -ge $MaxLatencyMs) {
        throw "WebUI asset $Path exceeded the ${MaxLatencyMs}ms latency budget (${elapsedMs}ms)."
    }

    return [pscustomobject]@{
        Path = $Path
        ElapsedMs = $elapsedMs
        Bytes = $bytes
    }
}


function Invoke-WebUiBatch {
    param(
        [string]$Base,
        [int]$TimeoutSec,
        [int]$MaxAssetLatency,
        [int]$MaxColdTlsLatency
    )

    $results = @()
    $results += Invoke-WebUiAsset $Base "/" $TimeoutSec $MaxColdTlsLatency
    $results += Invoke-WebUiAsset $Base "/assets/app.css" $TimeoutSec $MaxAssetLatency
    $results += Invoke-WebUiAsset $Base "/assets/app.js" $TimeoutSec $MaxAssetLatency
    return $results
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
        $status = Invoke-JsonRequest ("{0}/api/status" -f $Base) "GET" $null $TimeoutSec
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
    throw "measurement_start did not reach RECORDING for $ExpectedObjectId within ${TimeoutSec}s."
}


function Assert-MeasurementStartWorkflowReady {
    param(
        [string]$Base,
        [int]$RequestTimeoutSec,
        [string]$Action
    )

    $status = Invoke-JsonRequest ("{0}/api/status" -f $Base) "GET" $null $RequestTimeoutSec
    $step = [string]$status.workflow.step
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
        [int]$TimeoutSec,
        [int]$PollMs,
        [int]$RequestTimeoutSec
    )

    $encodedObjectId = [uri]::EscapeDataString($ExpectedObjectId)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $status = Invoke-JsonRequest ("{0}/api/status?sessions={1}" -f $Base, $encodedObjectId) "GET" $null $RequestTimeoutSec
        $recording = $status.recording
        $sessions = @($status.recording_sessions.sessions)
        $valid = @(
            $sessions | Where-Object {
                $_.session_id -eq $ExpectedSessionId -and
                $_.complete -eq $true -and
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
    throw "measurement_stop did not reach IDLE with complete zero-drop session $ExpectedSessionId within ${TimeoutSec}s."
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


function Get-MeasurementHwmStats {
    param([string]$Content)

    $matches = [regex]::Matches(
        $Content,
        "(?im)STACK_HWM\s+task=measurement_service\s+stage=(?<stage>[A-Za-z0-9_-]+)\s+free=(?<free>\d+)"
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


function Get-MemoryRuntimeStats {
    param([string]$Content)

    $fields = @(
        "internal_free",
        "internal_largest",
        "dma_free",
        "dma_largest",
        "psram_free",
        "psram_largest",
        "psram_dma_free",
        "psram_dma_largest"
    )
    $minimums = @{}
    foreach ($field in $fields) {
        $minimums[$field] = $null
    }

    $matches = [regex]::Matches(
        $Content,
        "(?im)MEM_RUNTIME\s+stage=\S+\s+internal_free=(?<internal_free>\d+)\s+internal_largest=(?<internal_largest>\d+)\s+dma_free=(?<dma_free>\d+)\s+dma_largest=(?<dma_largest>\d+)\s+psram_free=(?<psram_free>\d+)\s+psram_largest=(?<psram_largest>\d+)\s+psram_dma_free=(?<psram_dma_free>\d+)\s+psram_dma_largest=(?<psram_dma_largest>\d+)"
    )

    foreach ($match in $matches) {
        foreach ($field in $fields) {
            $group = $match.Groups[$field]
            if (-not $group.Success) {
                continue
            }
            $value = [uint64]$group.Value
            if ($null -eq $minimums[$field] -or $value -lt $minimums[$field]) {
                $minimums[$field] = $value
            }
        }
    }

    return [pscustomobject]@{
        Count = $matches.Count
        MinimumInternalFree = if ($null -eq $minimums["internal_free"]) { -1 } else { $minimums["internal_free"] }
        MinimumInternalLargest = if ($null -eq $minimums["internal_largest"]) { -1 } else { $minimums["internal_largest"] }
        MinimumDmaFree = if ($null -eq $minimums["dma_free"]) { -1 } else { $minimums["dma_free"] }
        MinimumDmaLargest = if ($null -eq $minimums["dma_largest"]) { -1 } else { $minimums["dma_largest"] }
        MinimumPsramFree = if ($null -eq $minimums["psram_free"]) { -1 } else { $minimums["psram_free"] }
        MinimumPsramLargest = if ($null -eq $minimums["psram_largest"]) { -1 } else { $minimums["psram_largest"] }
        MinimumPsramDmaFree = if ($null -eq $minimums["psram_dma_free"]) { -1 } else { $minimums["psram_dma_free"] }
        MinimumPsramDmaLargest = if ($null -eq $minimums["psram_dma_largest"]) { -1 } else { $minimums["psram_dma_largest"] }
    }
}


function Get-AssetTimingsFromLog {
    param([string]$Content)

    $timings = @()
    $matches = [regex]::Matches(
        $Content,
        "(?im)WEBUI_ASSET\s+path=(?<path>\S+)\s+elapsed_ms=(?<elapsed>\d+)"
    )
    foreach ($match in $matches) {
        $timings += [pscustomobject]@{
            Path = $match.Groups["path"].Value
            ElapsedMs = [int]$match.Groups["elapsed"].Value
        }
    }
    return $timings
}


function Test-TargetLog {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Content,
        [int]$RequiredCycles,
        [int]$MinimumHwmBytes,
        [int]$MaxAssetLatency,
        [int]$MaxColdTlsLatency,
        [int]$MinimumInternalFree,
        [int]$MinimumInternalLargest,
        [int]$MinimumDmaFree,
        [int]$MinimumDmaLargest,
        [int]$MinimumPsramFree,
        [object[]]$ObservedAssetTimings = $null
    )

    if ([string]::IsNullOrWhiteSpace($Content)) {
        Add-Check $Checks "Target serial log" "FAIL" "The log is empty or could not be read."
        return
    }

    $stats = Get-MeasurementHwmStats $Content
    Add-Check $Checks "Measurement HWM safety margin" `
        $(if ($stats.Count -gt 0 -and $stats.MinimumFree -ge $MinimumHwmBytes) { "PASS" } else { "FAIL" }) `
        ("minimum measurement_service free={0} bytes; required >= {1} bytes across {2} samples." -f $stats.MinimumFree, $MinimumHwmBytes, $stats.Count)

    foreach ($stage in @("before-start", "after-recorder-start", "after-stop")) {
        $count = if ($stats.StageCounts.ContainsKey($stage)) { $stats.StageCounts[$stage] } else { 0 }
        Add-Check $Checks ("STACK_HWM {0}" -f $stage) `
            $(if ($count -ge $RequiredCycles) { "PASS" } else { "FAIL" }) `
            ("Observed {0}; required at least {1}." -f $count, $RequiredCycles)
    }

    $reviewCount = Count-Pattern $Content "(?im)MEASUREMENT_SERVICE:\s+Recording stopped state=REVIEW"
    Add-Check $Checks "Measurement state REVIEW" `
        $(if ($reviewCount -ge $RequiredCycles) { "PASS" } else { "FAIL" }) `
        ("Observed {0} REVIEW completions; required at least {1}." -f $reviewCount, $RequiredCycles)

    $memory = Get-MemoryRuntimeStats $Content
    Write-Host ("MEMORY_HIL_MIN internal_free={0} internal_largest={1} dma_free={2} dma_largest={3} psram_free={4} psram_largest={5} psram_dma_free={6} psram_dma_largest={7} samples={8}" -f `
        $memory.MinimumInternalFree,
        $memory.MinimumInternalLargest,
        $memory.MinimumDmaFree,
        $memory.MinimumDmaLargest,
        $memory.MinimumPsramFree,
        $memory.MinimumPsramLargest,
        $memory.MinimumPsramDmaFree,
        $memory.MinimumPsramDmaLargest,
        $memory.Count)
    Add-Check $Checks "Memory telemetry coverage" `
        $(if ($memory.Count -gt 0 -and $memory.MinimumPsramDmaFree -ge 0 -and $memory.MinimumPsramDmaLargest -ge 0) { "PASS" } else { "FAIL" }) `
        ("Observed {0} complete MEM_RUNTIME snapshot(s); all internal/DMA/PSRAM/PSRAM-DMA minimums are reported above." -f $memory.Count)
    Add-Check $Checks "Minimum internal free" `
        $(if ($memory.Count -gt 0 -and $memory.MinimumInternalFree -ge $MinimumInternalFree) { "PASS" } else { "FAIL" }) `
        ("minimum={0} bytes; required >= {1} bytes." -f $memory.MinimumInternalFree, $MinimumInternalFree)
    Add-Check $Checks "Minimum internal largest block" `
        $(if ($memory.Count -gt 0 -and $memory.MinimumInternalLargest -ge $MinimumInternalLargest) { "PASS" } else { "FAIL" }) `
        ("minimum={0} bytes; required >= {1} bytes." -f $memory.MinimumInternalLargest, $MinimumInternalLargest)
    Add-Check $Checks "Minimum DMA free" `
        $(if ($memory.Count -gt 0 -and $memory.MinimumDmaFree -ge $MinimumDmaFree) { "PASS" } else { "FAIL" }) `
        ("minimum={0} bytes; required >= {1} bytes." -f $memory.MinimumDmaFree, $MinimumDmaFree)
    Add-Check $Checks "Minimum DMA largest block" `
        $(if ($memory.Count -gt 0 -and $memory.MinimumDmaLargest -ge $MinimumDmaLargest) { "PASS" } else { "FAIL" }) `
        ("minimum={0} bytes; required >= {1} bytes." -f $memory.MinimumDmaLargest, $MinimumDmaLargest)
    Add-Check $Checks "Minimum PSRAM free" `
        $(if ($memory.Count -gt 0 -and $memory.MinimumPsramFree -ge $MinimumPsramFree) { "PASS" } else { "FAIL" }) `
        ("minimum={0} bytes; required >= {1} bytes." -f $memory.MinimumPsramFree, $MinimumPsramFree)

    foreach ($marker in @("RUNTIME_CPU stage=", "MEM_RUNTIME stage=", "RUNTIME_TASK name=measurement_service")) {
        $count = Count-Pattern $Content ([regex]::Escape($marker))
        Add-Check $Checks ("Target marker {0}" -f $marker.TrimEnd("=")) `
            $(if ($count -gt 0) { "PASS" } else { "FAIL" }) `
            ("Observed {0} marker(s)." -f $count)
    }

    $assetTimings = if ($null -ne $ObservedAssetTimings -and @($ObservedAssetTimings).Count -gt 0) {
        @($ObservedAssetTimings)
    } else {
        @(Get-AssetTimingsFromLog $Content)
    }
    $requiredAssetMarkers = $RequiredCycles * 3
    $assetViolations = @(
        $assetTimings | Where-Object {
            $limit = if ($_.Path -eq "/") { $MaxColdTlsLatency } else { $MaxAssetLatency }
            [int]$_.ElapsedMs -ge $limit
        }
    )
    Add-Check $Checks "WebUI asset marker coverage" `
        $(if ($assetTimings.Count -ge $requiredAssetMarkers) { "PASS" } else { "FAIL" }) `
        ("Observed {0}; required at least {1} asset timings." -f $assetTimings.Count, $requiredAssetMarkers)
    Add-Check $Checks "WebUI asset latency" `
        $(if ($assetTimings.Count -ge $requiredAssetMarkers -and $assetViolations.Count -eq 0) { "PASS" } else { "FAIL" }) `
        $(if ($assetViolations.Count -eq 0) {
            "All measured assets stayed below cold_root={0}ms and subsequent={1}ms." -f $MaxColdTlsLatency, $MaxAssetLatency
        } else {
            $firstViolation = $assetViolations[0]
            "Asset {0} reached {1}ms; limit={2}ms." -f $firstViolation.Path, $firstViolation.ElapsedMs, $(if ($firstViolation.Path -eq "/") { $MaxColdTlsLatency } else { $MaxAssetLatency })
        })

    $panic = [regex]::Match(
        $Content,
        "(?im)(stack overflow|guru meditation|abort\(\)|corrupt heap|stack smashing|panic'ed|watchdog|task watchdog|\bWDT\b|brownout detector was triggered|not enough mem|TLS.*(OOM|NO_MEM)|SD.*(OOM|NO_MEM))"
    )
    Add-Check $Checks "Panic/watchdog/brownout/OOM detection" `
        $(if (-not $panic.Success) { "PASS" } else { "FAIL" }) `
        $(if (-not $panic.Success) { "No panic, stack overflow, watchdog, brownout, TLS OOM, or SD OOM marker observed." } else { "Failure marker: $($panic.Value.Trim())." })

    $resetCount = Count-Pattern $Content "(?im)^\s*rst:\s*0x[0-9a-f]+"
    Add-Check $Checks "Unexpected reboot detection" `
        $(if ($resetCount -le 1) { "PASS" } else { "FAIL" }) `
        ("Observed {0} boot reset markers; expected at most one initial boot." -f $resetCount)

    $resourceFailure = [regex]::Match(
        $Content,
        "(?im)(failed to close uncommitted session|failed to remove uncommitted session|storage is not ready|failed to flush|failed to write samples|lvgl.*(timeout|deadlock|stuck|failed)|lock.*(stuck|deadlock|timeout))"
    )
    Add-Check $Checks "Storage/session/LVGL cleanup" `
        $(if (-not $resourceFailure.Success) { "PASS" } else { "FAIL" }) `
        $(if (-not $resourceFailure.Success) { "No session-handle, storage-lock, or LVGL-lock failure marker observed." } else { "Failure marker: $($resourceFailure.Value.Trim())." })
}


$repo = Resolve-RepoPath "."
$checks = [System.Collections.Generic.List[object]]::new()
$monitor = $null
$logFile = $null
$stderrFile = $null
$base = if ([string]::IsNullOrWhiteSpace($BaseUrl)) { "" } else { $BaseUrl.TrimEnd("/") }
$previousActiveId = $null
$isReplay = -not [string]::IsNullOrWhiteSpace($LogPath)
$assetTimings = [System.Collections.Generic.List[object]]::new()
$liveAssertionsStarted = $false

try {
    if ($isReplay) {
        $logFile = (Resolve-Path -LiteralPath $LogPath).Path
        Test-TargetLog $checks (Read-TargetLog $logFile) $Cycles $MinimumMeasurementServiceHwmBytes `
            $MaxAssetLatencyMs $MaxColdTlsLatencyMs `
            $MinimumInternalFreeBytes $MinimumInternalLargestFreeBlockBytes `
            $MinimumDmaFreeBytes $MinimumDmaLargestFreeBlockBytes $MinimumPsramFreeBytes
        Add-Check $checks "Live WebUI/TLS/recording execution" "BLOCKED" "-LogPath validates captured markers only; it cannot prove live TLS, SD, WebUI, or target scheduling."
        Add-Check $checks "User-data safety" "PASS" "Replay mode performs no HTTP mutation, session delete, SD erase, or flash."
    } else {
        if ([string]::IsNullOrWhiteSpace($base) -or [string]::IsNullOrWhiteSpace($ObjectId)) {
            throw "Live mode requires -BaseUrl and -ObjectId for a pre-created dedicated HIL object."
        }

        $checkPorts = Join-Path $repo "tools/check_serial_ports.ps1"
        & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $checkPorts -Ports $LogPort
        if ($LASTEXITCODE -ne 0) {
            throw "Required HIL serial port preflight failed for $LogPort."
        }
        Add-Check $checks "Live serial preflight" "PASS" ("Live log port available: {0}." -f $LogPort)

        $hilDir = Join-Path $repo "build\hil"
        New-Item -ItemType Directory -Path $hilDir -Force | Out-Null
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $logFile = Join-Path $hilDir ("webui-memory-{0}.log" -f $stamp)
        $stderrFile = Join-Path $hilDir ("webui-memory-{0}.err.log" -f $stamp)
        $monitor = Start-SerialMonitor $logFile $stderrFile $ProjectPath $BuildDir $LogPort
        Start-Sleep -Seconds 2
        if ($monitor.HasExited) {
            throw "The target monitor exited before the HIL run. See $stderrFile."
        }

        Add-Check $checks "Production flash" "SKIPPED" "This runner never flashes; observe firmware installed by a separately authorized operator action."

        if (-not (Wait-ForLogCount $logFile "(?im)STACK_HWM\s+task=measurement_service\s+stage=idle\s+free=\d+" 1 $BootTimeoutSeconds $PollIntervalMs)) {
            throw "Instrumented measurement_service idle HWM marker was not observed after boot."
        }
        Add-Check $checks "Instrumented target boot" "PASS" "measurement_service emitted an idle STACK_HWM marker."
        $liveAssertionsStarted = $true

        $objectsStatus = Invoke-JsonRequest ("{0}/api/status?objects=1" -f $base) "GET" $null $RequestTimeoutSeconds
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
        $activate = Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $activateBody $RequestTimeoutSeconds
        if ($activate.ok -ne $true) {
            throw "Dedicated HIL object activation was rejected: $($activate.error)"
        }
        Add-Check $checks "Dedicated test object" "PASS" ("Using pre-created $ObjectId named '$($target.name)'; no user object is deleted or erased.")

        $initialStatus = Invoke-JsonRequest ("{0}/api/status?sessions={1}" -f $base, [uri]::EscapeDataString($ObjectId)) "GET" $null $RequestTimeoutSeconds
        if ($initialStatus.recording.running -eq $true) {
            throw "Target is already recording; refusing to interrupt it."
        }
        if ([int]$initialStatus.recording.last_error -ne 0) {
            throw "Target recorder starts in error state $($initialStatus.recording.last_error); refusing to mask it."
        }

        $seenSessions = [System.Collections.Generic.HashSet[string]]::new()
        for ($cycle = 1; $cycle -le $Cycles; ++$cycle) {
            $assetTimings.AddRange(@(Invoke-WebUiBatch $base $RequestTimeoutSeconds $MaxAssetLatencyMs $MaxColdTlsLatencyMs))
            Assert-MeasurementStartWorkflowReady $base $RequestTimeoutSeconds "measurement_start"
            $startBody = @{ action = "measurement_start"; object_id = $ObjectId } | ConvertTo-Json -Compress
            $start = Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $startBody $RequestTimeoutSeconds
            if ($start.ok -ne $true) {
                throw "cycle $cycle measurement_start rejected: $($start.error)"
            }

            $sessionId = Wait-RecordingStart $base $ObjectId $RequestTimeoutSeconds $PollIntervalMs
            if (-not $seenSessions.Add($sessionId)) {
                throw "cycle $cycle reused session ID $sessionId."
            }

            $recordUntil = (Get-Date).AddSeconds($RecordSeconds)
            $nextAssets = Get-Date
            do {
                $status = Invoke-JsonRequest ("{0}/api/status" -f $base) "GET" $null $RequestTimeoutSeconds
                if ($status.recording.state -ne "RECORDING" -or $status.recording.running -ne $true) {
                    throw "cycle $cycle left RECORDING before the $RecordSeconds second workload window."
                }
                if ([uint64]$status.recording.dropped_frames -gt 0) {
                    throw "cycle $cycle observed recorder dropped_frames=$($status.recording.dropped_frames)."
                }
                if ((Get-Date) -ge $nextAssets) {
                    $assetTimings.AddRange(@(Invoke-WebUiBatch $base $RequestTimeoutSeconds $MaxAssetLatencyMs $MaxColdTlsLatencyMs))
                    $nextAssets = (Get-Date).AddSeconds($AssetIntervalSeconds)
                }
                if ((Get-Date) -lt $recordUntil) {
                    Start-Sleep -Milliseconds $PollIntervalMs
                }
            } while ((Get-Date) -lt $recordUntil)

            $stopBody = @{ action = "measurement_stop" } | ConvertTo-Json -Compress
            $stop = Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $stopBody $RequestTimeoutSeconds
            if ($stop.ok -ne $true) {
                throw "cycle $cycle measurement_stop rejected: $($stop.error)"
            }

            $finalStatus = Wait-RecordingStop $base $ObjectId $sessionId $StopTimeoutSeconds $PollIntervalMs $RequestTimeoutSeconds
            $assetTimings.AddRange(@(Invoke-WebUiBatch $base $RequestTimeoutSeconds $MaxAssetLatencyMs $MaxColdTlsLatencyMs))
            if (-not (Wait-ForLogCount $logFile "(?im)MEASUREMENT_SERVICE:\s+Recording stopped state=REVIEW" $cycle $BootTimeoutSeconds $PollIntervalMs)) {
                throw "cycle $cycle did not produce the measurement_service REVIEW marker."
            }
            if ($cycle -lt $Cycles) {
                Invoke-WorkflowRecordAgain $base $RequestTimeoutSeconds $PollIntervalMs
            }
            Write-Host ("cycle {0}/{1}: session={2} complete, recording+WebUI workload passed" -f $cycle, $Cycles, $sessionId)
        }

        if ($seenSessions.Count -ne $Cycles) {
            throw "Expected $Cycles unique session IDs but observed $($seenSessions.Count)."
        }
        $maxAsset = @($assetTimings | Sort-Object ElapsedMs -Descending | Select-Object -First 1)
        if ($maxAsset.Count -eq 0 -or $maxAsset[0].ElapsedMs -ge ($RequestTimeoutSeconds * 1000)) {
            throw "No acceptable WebUI asset timing was collected, or the slowest request exceeded the request budget."
        }
        Add-Check $checks "Concurrent WebUI/TLS workload" "PASS" ("Completed {0} cycle(s); {1} asset requests overlapped recording, slowest={2}ms." -f $Cycles, $assetTimings.Count, $maxAsset[0].ElapsedMs)
        Add-Check $checks "Session/file handle cleanup" "PASS" ("Every cycle reached IDLE with a complete zero-drop session; no delete/erase was issued.")

        Test-TargetLog $checks (Read-TargetLog $logFile) $Cycles $MinimumMeasurementServiceHwmBytes `
            $MaxAssetLatencyMs $MaxColdTlsLatencyMs `
            $MinimumInternalFreeBytes $MinimumInternalLargestFreeBlockBytes `
            $MinimumDmaFreeBytes $MinimumDmaLargestFreeBlockBytes $MinimumPsramFreeBytes `
            @($assetTimings)
    }
} catch {
    Add-Check $checks "Runner execution" $(if ($isReplay -or $liveAssertionsStarted) { "FAIL" } else { "BLOCKED" }) $_.Exception.Message
} finally {
    if (-not $isReplay -and $null -ne $monitor -and -not $monitor.HasExited) {
        Stop-Process -Id $monitor.Id -Force -ErrorAction SilentlyContinue
        $monitor.WaitForExit()
    }

    if (-not $isReplay -and -not [string]::IsNullOrWhiteSpace($base) -and -not [string]::IsNullOrWhiteSpace($previousActiveId) -and $previousActiveId -ne $ObjectId) {
        try {
            $restoreBody = @{ action = "activate_object"; object_id = $previousActiveId } | ConvertTo-Json -Compress
            $restore = Invoke-JsonRequest ("{0}/api/control" -f $base) "POST" $restoreBody $RequestTimeoutSeconds
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

Write-Host "===== WebUI memory HIL report ====="
Write-Host ("{0,-46} {1,-9} {2}" -f "CHECK", "STATUS", "DETAIL")
Write-Host ("{0,-46} {1,-9} {2}" -f "-----", "------", "------")
foreach ($check in $checks) {
    Write-Host ("{0,-46} {1,-9} {2}" -f $check.Name, $check.Status, $check.Detail)
}
if ($logFile) {
    Write-Host ("LOG_PATH: {0}" -f $logFile)
}
Write-Host "SAFETY: this runner never flashes, deletes sessions, erases SD, or creates objects; it requires a pre-created HIL_MEMORY_TEST object."
Write-Host ("WEBUI_MEMORY_HIL_RESULT: {0}" -f $result)

if ($result -eq "PASS") {
    exit 0
}
exit 1
