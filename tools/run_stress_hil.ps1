[CmdletBinding()]
param(
    [string]$FlashPort = "COM4",
    [string]$LogPort = "COM6",
    [string]$ProjectPath = "tests/target/stress_app",
    [string]$BuildDir = "build-s3",
    [switch]$SkipBuild,
    [switch]$SkipFlash,
    [switch]$WaitForPowerHandoff,
    [string]$LogPath,
    [switch]$RequirePhysical,
    [switch]$ConfirmAd7606,
    [switch]$ConfirmSpi2Sd,
    [switch]$ConfirmTft,
    [switch]$ConfirmWifi,
    [switch]$ConfirmI2c,
    [ValidateRange(5, 900)]
    [int]$TimeoutSeconds = 180,
    [ValidateRange(100, 10000)]
    [int]$PollIntervalMs = 500
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

    [void]$Checks.Add([pscustomobject]@{
        Name = $Name
        Status = $Status
        Detail = $Detail
    })
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


function Read-Log {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }
    return Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
}


function Stop-ProcessTree {
    param([int]$RootProcessId)

    $pending = [System.Collections.Generic.Queue[int]]::new()
    $ids = [System.Collections.Generic.HashSet[int]]::new()
    $pending.Enqueue($RootProcessId)
    [void]$ids.Add($RootProcessId)

    while ($pending.Count -gt 0) {
        $parentId = $pending.Dequeue()
        foreach ($child in @(Get-CimInstance Win32_Process -Filter ("ParentProcessId = {0}" -f $parentId))) {
            if ($ids.Add([int]$child.ProcessId)) {
                $pending.Enqueue([int]$child.ProcessId)
            }
        }
    }

    foreach ($processId in @($ids | Sort-Object -Descending)) {
        Stop-Process -Id $processId -Force -ErrorAction SilentlyContinue
    }
}


function Add-TargetStressChecks {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Content,
        [bool]$Replay
    )

    if ([string]::IsNullOrWhiteSpace($Content)) {
        Add-Check $Checks "Stress UART log" $(if ($Replay) { "FAIL" } else { "BLOCKED" }) "The target stress log is empty or was not captured."
        return
    }

    $identity = [regex]::Match($Content, "(?im)TARGET_STRESS_BEGIN\s+seed=\d+\s+core_count=2\b")
    if ($identity.Success) {
        Add-Check $Checks "Target stress identity" "PASS" "UART contains the ESP32-S3 stress application identity marker."
    } else {
        Add-Check $Checks "Target stress identity" $(if ($Replay) { "FAIL" } else { "BLOCKED" }) "TARGET_STRESS_BEGIN for the stress application was not observed; do not attribute this UART stream to the stress image."
    }

    $summary = [regex]::Match($Content, "(?im)TARGET_STRESS_SUMMARY:\s*(PASS|FAIL)")
    if (-not $summary.Success) {
        Add-Check $Checks "Target stress summary" $(if ($Replay) { "FAIL" } else { "BLOCKED" }) "TARGET_STRESS_SUMMARY was not observed before the timeout."
    } elseif ($summary.Groups[1].Value -eq "PASS") {
        Add-Check $Checks "Target stress summary" "PASS" "All target-side stress phases reported PASS."
    } else {
        Add-Check $Checks "Target stress summary" "FAIL" "The target stress application reported FAIL."
    }

    $expectedSweepRates = @(100, 250, 500, 1000, 2000, 4000, 5000)
    $sweepMissing = [System.Collections.Generic.List[string]]::new()
    foreach ($rate in $expectedSweepRates) {
        $expectedOutcome = if ($rate -eq 5000) { "SATURATED" } else { "PASS" }
        $sweepPattern = "(?im)TARGET_STRESS_SCENARIO\s+name=rate_sweep\b[^\r\n]*\brate_hz=$rate\b[^\r\n]*\bexpected=$expectedOutcome\b[^\r\n]*\bverdict=PASS\b"
        if (-not [regex]::IsMatch($Content, $sweepPattern)) {
            [void]$sweepMissing.Add(("{0}Hz/{1}" -f $rate, $expectedOutcome))
        }
    }
    if ($sweepMissing.Count -eq 0) {
        Add-Check $Checks "Automatic rate sweep" "PASS" "Observed all 7 rate-sweep phases; 5000 Hz is the expected saturation boundary control."
    } else {
        Add-Check $Checks "Automatic rate sweep" "FAIL" ("Missing passing rate-sweep phases: {0}." -f ($sweepMissing -join ", "))
    }

    $binary = [regex]::Match($Content, "(?im)TARGET_STRESS_BINARY_SEARCH\s+seed=\d+\s+healthy_rate_hz=(\d+)\s+first_failed_rate_hz=(\d+)")
    if ($binary.Success -and [int]$binary.Groups[2].Value -gt [int]$binary.Groups[1].Value) {
        Add-Check $Checks "Binary-search saturation boundary" "PASS" ("Healthy <= {0} Hz; first failed boundary {1} Hz." -f $binary.Groups[1].Value, $binary.Groups[2].Value)
    } else {
        Add-Check $Checks "Binary-search saturation boundary" "FAIL" "A valid healthy/first-failed binary-search boundary was not observed."
    }

    $schedule = [regex]::Match($Content, "(?im)TARGET_STRESS_SCHEDULE\s+seed=\d+\s+events=(\d+)\s+inversions=(\d+)\s+trace_hash=\d+\s+replay=(PASS|FAIL)")
    if (
        $schedule.Success -and
        [UInt64]$schedule.Groups[1].Value -ge 64 -and
        [UInt64]$schedule.Groups[2].Value -gt 0 -and
        $schedule.Groups[3].Value -eq "PASS"
    ) {
        Add-Check $Checks "Evil scheduler replay" "PASS" ("Observed {0} deterministically replayed events with {1} inversions." -f $schedule.Groups[1].Value, $schedule.Groups[2].Value)
    } else {
        Add-Check $Checks "Evil scheduler replay" "FAIL" "A deterministic reordered schedule with a passing replay was not observed."
    }

    $saturation = [regex]::Match($Content, "(?im)TARGET_STRESS_SCENARIO\s+name=intentional_saturation\b[^\r\n]*observed=SATURATED\b[^\r\n]*expected=SATURATED\b[^\r\n]*verdict=PASS\b")
    if ($saturation.Success) {
        Add-Check $Checks "Intentional saturation control" "PASS" "The slow-SD/high-rate control phase saturated as expected."
    } else {
        Add-Check $Checks "Intentional saturation control" "FAIL" "The intentional saturation control phase was missing or did not saturate."
    }

    $isr = [regex]::Match($Content, "(?im)TARGET_STRESS_ISR\s+period_us=500\s+ticks=(\d+)\s+expected_min=(\d+)\s+status=(PASS|FAIL)")
    if (
        $isr.Success -and
        $isr.Groups[3].Value -eq "PASS" -and
        [UInt64]$isr.Groups[1].Value -ge [UInt64]$isr.Groups[2].Value
    ) {
        Add-Check $Checks "ISR timing" "PASS" ("The 500 us ISR-dispatch timer reported {0} ticks (minimum {1})." -f $isr.Groups[1].Value, $isr.Groups[2].Value)
    } else {
        Add-Check $Checks "ISR timing" "FAIL" "The target ISR timing marker was missing, failed, or reported too few ticks."
    }

    $stackTasks = @(
        [regex]::Matches($Content, "(?im)TARGET_STRESS_STACK\s+task=(producer|recorder|ui|sensor|network|scheduler)\b") |
            ForEach-Object { $_.Groups[1].Value } |
            Sort-Object -Unique
    )
    if ($stackTasks.Count -eq 6) {
        Add-Check $Checks "FreeRTOS stack HWM" "PASS" ("Stack high-water marks observed for: {0}." -f ($stackTasks -join ", "))
    } else {
        Add-Check $Checks "FreeRTOS stack HWM" "FAIL" ("Expected 6 task stack HWM markers; observed {0}: {1}." -f $stackTasks.Count, ($stackTasks -join ", "))
    }

    $heapStarts = @([regex]::Matches($Content, "(?im)TARGET_STRESS_HEAP\s+stage=start\b")).Count
    $heapEnds = @([regex]::Matches($Content, "(?im)TARGET_STRESS_HEAP\s+stage=end\b")).Count
    if ($heapStarts -gt 0 -and $heapEnds -gt 0) {
        Add-Check $Checks "Internal/DMA/PSRAM heap telemetry" "PASS" ("Observed {0} start and {1} end heap snapshots." -f $heapStarts, $heapEnds)
    } else {
        Add-Check $Checks "Internal/DMA/PSRAM heap telemetry" "FAIL" "Start/end heap snapshots were not observed."
    }

    $traceCount = @([regex]::Matches($Content, "(?im)TARGET_STRESS_TRACE\s+seq=\d+")).Count
    if ($traceCount -gt 0) {
        Add-Check $Checks "Replayable scheduler trace" "PASS" ("Observed {0} scheduler/event trace records." -f $traceCount)
    } else {
        Add-Check $Checks "Replayable scheduler trace" "FAIL" "No TARGET_STRESS_TRACE records were observed."
    }

    $panic = [regex]::Match($Content, "(?im)(Guru Meditation|CORRUPT HEAP|Stack smashing|abort\(\)|Backtrace:)")
    if (-not $panic.Success) {
        Add-Check $Checks "Target panic/watchdog markers" "PASS" "No panic or heap-corruption marker was observed."
    } else {
        Add-Check $Checks "Target panic/watchdog markers" "FAIL" ("Failure marker observed: {0}." -f $panic.Value)
    }
}


function Add-PhysicalAttestation {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Content,
        [string]$Name,
        [string]$ProbeName,
        [bool]$Confirmed,
        [bool]$Replay,
        [bool]$Required,
        [string]$SwitchName,
        [string]$Instruction
    )

    if ($Replay -and $Confirmed) {
        Add-Check $Checks $Name "BLOCKED" ("-{0} cannot turn a saved UART log into current physical proof." -f $SwitchName)
        return
    }

    if ($Replay) {
        Add-Check $Checks $Name "SKIPPED" $Instruction
        return
    }

    $probePattern = "(?im)TARGET_STRESS_PHYSICAL\s+name={0}\b[^\r\n]*\bverdict=(PASS|FAIL)\b" -f [regex]::Escape($ProbeName)
    $probe = [regex]::Match($Content, $probePattern)
    if ($probe.Success -and $probe.Groups[1].Value -eq "FAIL") {
        if ($Required) {
            Add-Check $Checks $Name "FAIL" ("Target physical probe '{0}' reported FAIL. {1}" -f $ProbeName, $Instruction)
        } else {
            Add-Check $Checks $Name "SKIPPED" ("Target physical probe '{0}' reported FAIL; use -RequirePhysical for a physical gate." -f $ProbeName)
        }
        return
    }

    if (-not $probe.Success) {
        Add-Check $Checks $Name $(if ($Required) { "BLOCKED" } else { "SKIPPED" }) `
            ("Target physical probe '{0}' was not observed. {1}" -f $ProbeName, $Instruction)
        return
    }

    if ($Required -and -not $Confirmed) {
        Add-Check $Checks $Name "BLOCKED" ("Driver probe passed; live operator confirmation is still required with -{0}. {1}" -f $SwitchName, $Instruction)
    } elseif ($Required) {
        Add-Check $Checks $Name "PASS" ("Driver probe passed and operator attestation supplied: {0}" -f $Instruction)
    } else {
        Add-Check $Checks $Name "SKIPPED" ("Driver probe passed; use -RequirePhysical to enforce operator confirmation. {0}" -f $Instruction)
    }
}


$checks = [System.Collections.Generic.List[object]]::new()
$monitor = $null
$monitorRootProcessId = $null
$logFile = $null
$stderrFile = $null
$isReplay = -not [string]::IsNullOrWhiteSpace($LogPath)

try {
    $repo = Resolve-RepoPath "."
    $project = Resolve-RepoPath $ProjectPath
    $runIdf = Join-Path $repo "tools\run_idf.ps1"
    $checkPorts = Join-Path $repo "tools\check_serial_ports.ps1"
    $powershell = (Get-Command powershell.exe -ErrorAction Stop).Source
    if (-not (Test-Path -LiteralPath $runIdf -PathType Leaf)) { throw "Missing ESP-IDF helper: $runIdf" }
    if (-not (Test-Path -LiteralPath $checkPorts -PathType Leaf)) { throw "Missing serial-port helper: $checkPorts" }

    if ($isReplay) {
        $logFile = (Resolve-Path -LiteralPath $LogPath).Path
        Add-Check $checks "Stress build" "SKIPPED" "Replay mode; no build or hardware access."
        Add-Check $checks "Live serial preflight" "SKIPPED" "Replay mode; no hardware access."
        Add-Check $checks "Stress flash" "SKIPPED" "Replay mode; no flash access."
    } else {
        if ($RequirePhysical -and -not $WaitForPowerHandoff) {
            throw "-RequirePhysical requires -WaitForPowerHandoff: flash through COM4 first, move power to the USB 18650 block, then capture the post-handoff run on receive-only COM6."
        }
        $imagePath = if ([IO.Path]::IsPathRooted($BuildDir)) {
            Join-Path $BuildDir "geolocator_stress_app.bin"
        } else {
            Join-Path (Join-Path $project $BuildDir) "geolocator_stress_app.bin"
        }
        if ($SkipBuild) {
            if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) {
                throw "-SkipBuild supplied but the stress image is missing: $imagePath"
            }
            Add-Check $checks "Stress build" "SKIPPED" ("-SkipBuild supplied; using existing image {0}." -f $imagePath)
        } else {
            & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $runIdf `
                -Action build -ProjectPath $ProjectPath -BuildDir $BuildDir -Target esp32s3 -Jobs 2
            if ($LASTEXITCODE -ne 0) { throw "Stress target build failed with exit code $LASTEXITCODE." }
            Add-Check $checks "Stress build" "PASS" "ESP-IDF stress image built for ESP32-S3."
        }

        $preflightPorts = if ($SkipFlash) {
            @($LogPort)
        } else {
            @($FlashPort, $LogPort)
        }
        & $checkPorts -Ports ($preflightPorts -join ",")
        Add-Check $checks "Live serial preflight" "PASS" ("Ports available: {0}." -f ($preflightPorts -join ", "))

        $hilDir = Join-Path $repo "build\hil"
        New-Item -ItemType Directory -Path $hilDir -Force | Out-Null
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $logFile = Join-Path $hilDir ("stress-target-{0}.log" -f $stamp)
        $stderrFile = Join-Path $hilDir ("stress-target-{0}.err.log" -f $stamp)
        $monitorArgs = @(
            "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"{0}"' -f $runIdf),
            "-Action", "monitor", "-ProjectPath", $ProjectPath, "-BuildDir", $BuildDir, "-Port", $LogPort
        )
        if (-not $WaitForPowerHandoff) {
            $monitor = Start-Process -FilePath $powershell -ArgumentList $monitorArgs -WorkingDirectory $project `
                -RedirectStandardOutput $logFile -RedirectStandardError $stderrFile -PassThru -WindowStyle Hidden
            $monitorRootProcessId = $monitor.Id
            Start-Sleep -Seconds 2
            if ($monitor.HasExited) { throw "Stress UART monitor exited before the run. See $stderrFile." }
        }

        if ($SkipFlash) {
            Add-Check $checks "Stress flash" "SKIPPED" "-SkipFlash supplied; reset/power-cycle the board to run the already-built image."
        } else {
            & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $runIdf `
                -Action flash -ProjectPath $ProjectPath -BuildDir $BuildDir -Port $FlashPort
            if ($LASTEXITCODE -ne 0) { throw "Stress flash failed with exit code $LASTEXITCODE." }
            Add-Check $checks "Stress flash" "PASS" ("Flashed through {0}." -f $FlashPort)
        }

        if ($WaitForPowerHandoff) {
            Write-Host "POWER_HANDOFF_REQUIRED: unplug the same USB cable used for COM4 from the PC and connect it to the USB 18650 block. Keep the separate COM6 UART receive-only (no TX/VCC)."
            [void](Read-Host "After moving power, press Enter to open the COM6 UART monitor")
            $monitor = Start-Process -FilePath $powershell -ArgumentList $monitorArgs -WorkingDirectory $project `
                -RedirectStandardOutput $logFile -RedirectStandardError $stderrFile -PassThru -WindowStyle Hidden
            $monitorRootProcessId = $monitor.Id
            Start-Sleep -Seconds 2
            if ($monitor.HasExited) { throw "Stress UART monitor exited before the powered run. See $stderrFile." }
            [void](Read-Host "Press EN/RESET on the ESP32 now, then press Enter here; the runner will detect completion automatically")
        }
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $completion = "TIMEOUT"
    do {
        $content = Read-Log $logFile
        if (-not $isReplay -and $stderrFile) { $content += "`n" + (Read-Log $stderrFile) }
        $summary = [regex]::Match($content, "(?im)TARGET_STRESS_SUMMARY:\s*(PASS|FAIL)")
        if ($summary.Success) {
            $completion = "SUMMARY"
            Write-Host ("STRESS_RUN_COMPLETE: TARGET_STRESS_SUMMARY={0}" -f $summary.Groups[1].Value)
            break
        }
        $brownoutCount = @(
            [regex]::Matches($content, "(?im)(BROWNOUT_RST|Brownout detector was triggered|E BOD: Brownout)")
        ).Count
        if (-not $isReplay -and $brownoutCount -ge 2) {
            $completion = "BROWNOUT_ABORT"
            Write-Host ("BROWNOUT_ABORT: observed {0} brownout markers before TARGET_STRESS_SUMMARY." -f $brownoutCount)
            break
        }
        if ($isReplay -or ($null -ne $monitor -and $monitor.HasExited)) { break }
        if ((Get-Date) -lt $deadline) { Start-Sleep -Milliseconds $PollIntervalMs }
    } while ((Get-Date) -lt $deadline)

    if ($completion -eq "TIMEOUT") {
        Write-Host "STRESS_RUN_TIMEOUT: TARGET_STRESS_SUMMARY was not observed before the timeout."
    }

    Add-TargetStressChecks $checks $content ([bool]$isReplay)

    $brownoutMarkers = @(
        [regex]::Matches($content, "(?im)(BROWNOUT_RST|Brownout detector was triggered|E BOD: Brownout)")
    ).Count
    if ($brownoutMarkers -gt 0) {
        Add-Check $checks "Target power/brownout markers" "FAIL" ("Observed {0} brownout markers; this run is not valid stress completion evidence." -f $brownoutMarkers)
    } else {
        Add-Check $checks "Target power/brownout markers" "PASS" "No brownout reset marker was observed."
    }

    Add-PhysicalAttestation $checks $content "Physical AD7606 + SPI3" "ad7606" ([bool]$ConfirmAd7606) ([bool]$isReplay) ([bool]$RequirePhysical) `
        "ConfirmAd7606" "Observe the live pre-stress AD7606 conversions, DRDY, SPI3/DMA and non-flat channel payload; synthetic target stress metrics are reported separately."
    Add-PhysicalAttestation $checks $content "Physical shared SPI2 + SD" "spi2_sd" ([bool]$ConfirmSpi2Sd) ([bool]$isReplay) ([bool]$RequirePhysical) `
        "ConfirmSpi2Sd" "Observe the live pre-stress SD write-read diagnostics on the shared SPI2 bus; synthetic target recorder/cache results are reported separately."
    Add-PhysicalAttestation $checks $content "Physical TFT rendering" "tft" ([bool]$ConfirmTft) ([bool]$isReplay) ([bool]$RequirePhysical) `
        "ConfirmTft" "Observe the live pre-stress TFT DMA frames and panel response; this probe does not claim concurrent synthetic SPI2 contention."
    Add-PhysicalAttestation $checks $content "Physical Wi-Fi" "wifi" ([bool]$ConfirmWifi) ([bool]$isReplay) ([bool]$RequirePhysical) `
        "ConfirmWifi" "Observe the live Wi-Fi radio scans; association/API behavior remains a separate production-pipeline check."
    Add-PhysicalAttestation $checks $content "Physical I2C" "i2c" ([bool]$ConfirmI2c) ([bool]$isReplay) ([bool]$RequirePhysical) `
        "ConfirmI2c" "Observe the live pre-stress INA3221/TCA9548A transactions with no bus errors or lockups; synthetic target stress is reported separately."
} catch {
    Add-Check $checks "Runner execution" $(if ($isReplay) { "FAIL" } else { "BLOCKED" }) $_.Exception.Message
} finally {
    if ($null -ne $monitorRootProcessId) {
        Stop-ProcessTree ([int]$monitorRootProcessId)
    }
}

$failed = @($checks | Where-Object { $_.Status -eq "FAIL" })
$blocked = @($checks | Where-Object { $_.Status -eq "BLOCKED" })
$result = if ($failed.Count -gt 0) { "FAIL" } elseif ($blocked.Count -gt 0) { "BLOCKED" } else { "PASS" }

Write-Host "===== ESP32 target stress HIL report ====="
Write-Host ("{0,-38} {1,-9} {2}" -f "CHECK", "STATUS", "DETAIL")
Write-Host ("{0,-38} {1,-9} {2}" -f "-----", "------", "------")
foreach ($check in $checks) {
    Write-Host ("{0,-38} {1,-9} {2}" -f $check.Name, $check.Status, $check.Detail)
}
if ($isReplay) { Write-Host ("LOG_REPLAY: {0}" -f $logFile) }
if ($RequirePhysical) {
    Write-Host "PHYSICAL_CONDITION: production-driver probes plus operator attestations required for AD7606/SPI3, SPI2/SD, TFT, Wi-Fi and I2C."
} else {
    Write-Host "PHYSICAL_CONDITION: NOT_CONFIRMED (use -RequirePhysical and the five -Confirm* switches on a live board; driver probes are telemetry only)."
}
Write-Host ("TARGET_STRESS_HIL_RESULT: {0}" -f $result)

if ($result -eq "PASS") { exit 0 }
exit 1
