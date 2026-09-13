[CmdletBinding()]
param(
    [string]$FlashPort = "COM4",
    [string]$LogPort = "COM6",
    [string]$ProjectPath = ".",
    [string]$BuildDir = "build",
    [string]$LogPath,
    [switch]$SkipBuild,
    [switch]$SkipFlash,

    [ValidateRange(1, 120000)]
    [int]$MaxStartupMs = 5000,

    [ValidateRange(1, 10000)]
    [int]$MaxRecoveryWindowMs = 250,

    [ValidateRange(0, 1000)]
    [int]$MinimumRecoverySamples = 1,

    [ValidateRange(1, 1000)]
    [int]$MinimumKeypadDispatches = 5,

    [ValidateRange(5, 900)]
    [int]$TimeoutSeconds = 75,

    [ValidateRange(100, 5000)]
    [int]$PollIntervalMs = 250,

    [ValidateRange(1, 32)]
    [int]$Jobs = 2
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "boot_responsiveness_contract.ps1")


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


function Read-LogText {
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


function Resolve-RepositoryPath {
    param([string]$InputPath)

    $repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
    if ([IO.Path]::IsPathRooted($InputPath)) {
        return (Resolve-Path -LiteralPath $InputPath).Path
    }
    return (Resolve-Path -LiteralPath (Join-Path $repo $InputPath)).Path
}


function Format-Metric {
    param(
        [AllowNull()]
        [object]$Value,
        [string]$Suffix
    )

    if ($null -eq $Value) {
        return "missing"
    }
    return "$Value$Suffix"
}


$checks = [System.Collections.Generic.List[object]]::new()
$monitor = $null
$logFile = $null
$stderrFile = $null
$isReplay = -not [string]::IsNullOrWhiteSpace($LogPath)
$analysis = $null
$content = ""

try {
    $repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
    $runIdf = Join-Path $PSScriptRoot "run_idf.ps1"
    $checkPorts = Join-Path $PSScriptRoot "check_serial_ports.ps1"

    if ($isReplay) {
        $logFile = (Resolve-Path -LiteralPath $LogPath).Path
        Add-Check $checks "Live serial preflight" "SKIPPED" "Replay mode from -LogPath; no hardware access."
        Add-Check $checks "Production build" "SKIPPED" "Replay mode from -LogPath."
        Add-Check $checks "Production flash" "SKIPPED" "Replay mode from -LogPath."
    } else {
        if ($FlashPort -eq $LogPort) {
            throw "FlashPort and LogPort must be different. Use COM4 for programming and the passive USB-TTL port (normally COM6) for UART logs."
        }
        if (-not (Test-Path -LiteralPath $runIdf -PathType Leaf)) {
            throw "Missing ESP-IDF helper: $runIdf"
        }
        if (-not (Test-Path -LiteralPath $checkPorts -PathType Leaf)) {
            throw "Missing serial-port helper: $checkPorts"
        }

        $powershellCommand = Get-Command powershell.exe -ErrorAction SilentlyContinue
        if ($null -ne $powershellCommand) {
            $powershell = $powershellCommand.Source
        } else {
            $powershell = (Get-Command pwsh -ErrorAction Stop).Source
        }

        if ($SkipBuild) {
            Add-Check $checks "Production build" "SKIPPED" "-SkipBuild supplied; using existing build directory."
        } else {
            & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $runIdf `
                -Action build -ProjectPath $ProjectPath -BuildDir $BuildDir -Jobs $Jobs
            if ($LASTEXITCODE -ne 0) {
                throw "Production build failed with exit code $LASTEXITCODE."
            }
            Add-Check $checks "Production build" "PASS" ("Built production firmware in {0}." -f $BuildDir)
        }

        & $checkPorts -Ports ("{0},{1}" -f $FlashPort, $LogPort)
        if ($LASTEXITCODE -ne 0) {
            throw "Live $FlashPort/$LogPort preflight failed."
        }
        Add-Check $checks "Live serial preflight" "PASS" ("Live ports available: {0}, {1}." -f $FlashPort, $LogPort)

        $hilDir = Join-Path $repo "build\hil"
        New-Item -ItemType Directory -Path $hilDir -Force | Out-Null
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $logFile = Join-Path $hilDir ("boot-responsiveness-{0}.log" -f $stamp)
        $stderrFile = Join-Path $hilDir ("boot-responsiveness-{0}.err.log" -f $stamp)

        $project = Resolve-RepositoryPath $ProjectPath
        $monitorArgs = @(
            "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"{0}"' -f $runIdf),
            "-Action", "monitor", "-ProjectPath", $ProjectPath,
            "-BuildDir", $BuildDir, "-Port", $LogPort
        )
        Write-Host ("Starting passive UART monitor on {0}; output: {1}" -f $LogPort, $logFile)
        $monitor = Start-Process -FilePath $powershell -ArgumentList $monitorArgs `
            -WorkingDirectory $project -RedirectStandardOutput $logFile `
            -RedirectStandardError $stderrFile -PassThru -WindowStyle Hidden
        Start-Sleep -Seconds 2
        if ($monitor.HasExited) {
            throw "Passive UART monitor exited before boot HIL. See $stderrFile."
        }

        if ($SkipFlash) {
            Add-Check $checks "Production flash" "SKIPPED" "-SkipFlash supplied; reset/power-cycle the board now so COM6 captures a full boot."
        } else {
            & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $runIdf `
                -Action flash -ProjectPath $ProjectPath -BuildDir $BuildDir -Port $FlashPort
            if ($LASTEXITCODE -ne 0) {
                throw "Production flash failed with exit code $LASTEXITCODE."
            }
            Add-Check $checks "Production flash" "PASS" ("Flashed through {0}; logs remain passive on {1}." -f $FlashPort, $LogPort)
        }

        Write-Host "After the HOME screen appears, press LEFT, UP, DOWN, RIGHT and OK repeatedly. Keep pressing through background recovery and the following Web status-cache refresh until the report finishes."
        Write-Host ("Collecting up to {0}s of COM6 telemetry..." -f $TimeoutSeconds)

        $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
        do {
            $content = Read-LogText $logFile
            if ($stderrFile) {
                $content = $content + "`n" + (Read-LogText $stderrFile)
            }
            $analysis = Test-BootResponsivenessTelemetry `
                -Content $content `
                -MaxStartupMs $MaxStartupMs `
                -MaxRecoveryWindowMs $MaxRecoveryWindowMs `
                -MinimumRecoverySamples $MinimumRecoverySamples `
                -MinimumKeypadDispatches $MinimumKeypadDispatches

            $coreMarkersReady =
                $analysis.StartupCount -gt 0 -and
                $analysis.RecoveryCount -ge $MinimumRecoverySamples -and
                $analysis.KeypadDispatchCount -ge $MinimumKeypadDispatches -and
                $analysis.MissingKeyCodes.Count -eq 0 -and
                $content -match 'OBJECT_RECOVERY_BACKGROUND state=STOP' -and
                $content -match 'WEB_STATUS_CACHE refresh=done'

            if ($coreMarkersReady) {
                break
            }
            if ($monitor.HasExited) {
                break
            }
            if ((Get-Date) -lt $deadline) {
                Start-Sleep -Milliseconds $PollIntervalMs
            }
        } while ((Get-Date) -lt $deadline)
    }

    if ($isReplay) {
        $content = Read-LogText $logFile
    } else {
        $content = Read-LogText $logFile
        if ($stderrFile) {
            $content = $content + "`n" + (Read-LogText $stderrFile)
        }
    }

    $analysis = Test-BootResponsivenessTelemetry `
        -Content $content `
        -MaxStartupMs $MaxStartupMs `
        -MaxRecoveryWindowMs $MaxRecoveryWindowMs `
        -MinimumRecoverySamples $MinimumRecoverySamples `
        -MinimumKeypadDispatches $MinimumKeypadDispatches

    $missingStatus = "BLOCKED"
    $startupOk = $analysis.StartupCount -gt 0 -and $null -ne $analysis.MaxStartupMs -and $analysis.MaxStartupMs -le $MaxStartupMs
    Add-Check $checks "Interactive startup latency" `
        $(if ($startupOk) { "PASS" } elseif ($analysis.StartupCount -gt 0) { "FAIL" } else { $missingStatus }) `
        ("startup_total={0} limit={1}ms" -f (Format-Metric $analysis.MaxStartupMs "ms"), $MaxStartupMs)

    $recoveryOk =
        $analysis.RecoveryCount -ge $MinimumRecoverySamples -and
        $null -ne $analysis.MaxRecoveryWindowMs -and
        $analysis.MaxRecoveryWindowMs -le $MaxRecoveryWindowMs
    Add-Check $checks "Background recovery storage window" `
        $(if ($recoveryOk) { "PASS" } elseif ($analysis.RecoveryCount -gt 0) { "FAIL" } else { $missingStatus }) `
        ("samples={0} max={1} limit={2}ms removed_objects={3}" -f `
            $analysis.RecoveryCount, (Format-Metric $analysis.MaxRecoveryWindowMs "ms"), `
            $MaxRecoveryWindowMs, $analysis.RemovedObjectCount)

    $keypadOk =
        $analysis.KeypadDispatchCount -ge $MinimumKeypadDispatches -and
        $analysis.MissingKeyCodes.Count -eq 0
    $seenKeys = if ($analysis.SeenKeyCodes.Count -gt 0) { $analysis.SeenKeyCodes -join ',' } else { 'none' }
    $missingKeys = if ($analysis.MissingKeyCodes.Count -gt 0) { $analysis.MissingKeyCodes -join ',' } else { 'none' }
    Add-Check $checks "All five keypad keys reach LVGL" `
        $(if ($keypadOk) { "PASS" } else { $missingStatus }) `
        ("dispatches={0}/{1} keys_seen={2} missing={3} max_dispatch_age={4}" -f `
            $analysis.KeypadDispatchCount, $MinimumKeypadDispatches, $seenKeys, $missingKeys, `
            (Format-Metric $analysis.MaxKeypadDispatchAgeMs "ms"))

    Add-Check $checks "No stale keypad navigation" `
        $(if ($analysis.StaleDropCount -eq 0) { "PASS" } else { "FAIL" }) `
        ("stale_drops={0}" -f $analysis.StaleDropCount)

    Add-Check $checks "No spontaneous safe shutdown" `
        $(if ($analysis.SafeShutdownCount -eq 0) { "PASS" } else { "FAIL" }) `
        ("safe_shutdown_markers={0}" -f $analysis.SafeShutdownCount)

    Add-Check $checks "No target panic/watchdog/brownout" `
        $(if ($analysis.PanicCount -eq 0) { "PASS" } else { "FAIL" }) `
        ("failure_markers={0}" -f $analysis.PanicCount)

    $orderingViolations = @($analysis.Violations | Where-Object {
        $_ -like '*background recovery started before*' -or
        $_ -like '*Web status cache completed before*'
    })
    $orderingMissing = @($analysis.Missing | Where-Object {
        $_ -like 'OBJECT_RECOVERY_BACKGROUND*' -or
        $_ -eq 'WEB_STATUS_CACHE refresh=done'
    })
    Add-Check $checks "Startup -> recovery -> Web cache ordering" `
        $(if ($orderingViolations.Count -gt 0) { "FAIL" } elseif ($orderingMissing.Count -gt 0) { $missingStatus } else { "PASS" }) `
        $(if ($orderingViolations.Count -gt 0) { $orderingViolations -join '; ' } elseif ($orderingMissing.Count -gt 0) { "missing: " + ($orderingMissing -join ', ') } else { "background recovery begins after startup_total; Web cache completes after recovery stop" })

    $priorityViolations = @($analysis.Violations | Where-Object { $_ -like 'web_status_cache*' })
    if ($analysis.PrioritySampleCount -eq 0) {
        Add-Check $checks "Web cache priority inheritance diagnostic" "SKIPPED" `
            "No web_status_cache task entered the periodic top-task snapshot; this does not imply a priority failure."
    } elseif ($priorityViolations.Count -gt 0) {
        Add-Check $checks "Web cache priority inheritance diagnostic" "FAIL" ($priorityViolations -join '; ')
    } else {
        Add-Check $checks "Web cache priority inheritance diagnostic" "PASS" `
            ("samples={0} base_priority=1 inherited_observed={1}" -f $analysis.PrioritySampleCount, $analysis.InheritedObserved)
    }

    foreach ($violation in $analysis.Violations) {
        if (
            $violation -notin $orderingViolations -and
            $violation -notin $priorityViolations -and
            $violation -notlike 'startup_total=*' -and
            $violation -notlike 'recovery_window=*' -and
            $violation -notlike 'stale keypad drops*' -and
            $violation -notlike 'unexpected safe shutdown*' -and
            $violation -notlike 'panic/watchdog/brownout/assert*'
        ) {
            Add-Check $checks "Additional telemetry contract" "FAIL" $violation
        }
    }
} catch {
    Add-Check $checks "Runner execution" $(if ($isReplay) { "FAIL" } else { "BLOCKED" }) $_.Exception.Message
} finally {
    if ($null -ne $monitor -and -not $monitor.HasExited) {
        Stop-Process -Id $monitor.Id -Force -ErrorAction SilentlyContinue
        $monitor.WaitForExit()
    }
}

$functionalChecks = @($checks | Where-Object {
    $_.Name -notin @(
        "Live serial preflight",
        "Production build",
        "Production flash"
    )
})
$failed = @($functionalChecks | Where-Object { $_.Status -eq "FAIL" })
$blocked = @($functionalChecks | Where-Object { $_.Status -eq "BLOCKED" })
$telemetryResult = if ($failed.Count -gt 0) {
    "FAIL"
} elseif ($blocked.Count -gt 0) {
    "BLOCKED"
} else {
    "PASS"
}

$preflightChecks = @($checks | Where-Object {
    $_.Name -in @("Live serial preflight", "Production build", "Production flash")
})
$preflightFailed = @($preflightChecks | Where-Object { $_.Status -eq "FAIL" })
$preflightBlocked = @($preflightChecks | Where-Object { $_.Status -eq "BLOCKED" })
$hilResult = if ($telemetryResult -eq "FAIL" -or $preflightFailed.Count -gt 0) {
    "FAIL"
} elseif ($isReplay) {
    "BLOCKED"
} elseif ($telemetryResult -eq "BLOCKED" -or $preflightBlocked.Count -gt 0) {
    "BLOCKED"
} else {
    "PASS"
}

Write-Host "===== Boot responsiveness HIL report ====="
Write-Host ("{0,-43} {1,-9} {2}" -f "CHECK", "STATUS", "DETAIL")
Write-Host ("{0,-43} {1,-9} {2}" -f "-----", "------", "------")
foreach ($check in $checks) {
    Write-Host ("{0,-43} {1,-9} {2}" -f $check.Name, $check.Status, $check.Detail)
}
if ($logFile) {
    Write-Host ("LOG: {0}" -f $logFile)
}
if ($isReplay) {
    Write-Host "REPLAY_LIMITATION: Parsing a saved log never proves a physical keypad press, passive COM6 wiring, or current-board timing."
}
Write-Host ("BOOT_RESPONSIVENESS_TELEMETRY_RESULT: {0}" -f $telemetryResult)
Write-Host ("BOOT_RESPONSIVENESS_HIL_RESULT: {0}" -f $hilResult)

if ($hilResult -eq "PASS") {
    exit 0
}
exit 1
