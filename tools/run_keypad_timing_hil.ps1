[CmdletBinding()]
param(
    [ValidateSet("AD7606", "ESP_ADC_BENCH")]
    [string]$Source = "AD7606",

    [string]$FlashPort = "COM4",
    [string]$LogPort = "COM6",
    [string]$ProjectPath = ".",
    [string]$BuildDir,
    [switch]$SkipFlash,
    [string]$LogPath,
    [switch]$ConfirmFrequentKeypadActivity,
    [switch]$RequireFrequentKeypadActivity,

    [ValidateRange(1, 60)]
    [int]$MinimumWindows = 3,

    [ValidateRange(1, 1000000)]
    [int]$MinimumSnapshots = 100,

    [ValidateRange(5, 900)]
    [int]$TimeoutSeconds = 60,

    [ValidateRange(100, 10000)]
    [int]$PollIntervalMs = 500,

    [ValidateRange(100, 5000)]
    [int]$ExpectedSampleRateHz = 500,

    [ValidateRange(1, 32)]
    [int]$Jobs = 2
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "keypad_timing_contract.ps1")


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


function Read-TimingLog {
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


function Test-TimingBackendMarker {
    param(
        [string]$Content,
        [string]$SelectedSource,
        [int]$ExpectedRate
    )

    $pattern = if ($SelectedSource -eq "AD7606") {
        "(?im)AD7606[_ ]?ACQ:.*Started:\s*$ExpectedRate\s*Hz,\s*8\s*channels,\s*SPI3 dedicated,\s*core=1"
    } else {
        "(?im)GEOPHONE_ADC:.*ESP ADC bench enabled:.*at\s+$ExpectedRate\s+Hz\s+core=1"
    }

    if ($Content -match $pattern) {
        return [pscustomobject]@{
            Pass = $true
            Detail = "$SelectedSource backend marker observed at ${ExpectedRate}Hz."
        }
    }

    return [pscustomobject]@{
        Pass = $false
        Detail = "Required $SelectedSource backend marker at ${ExpectedRate}Hz was not observed."
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


function Restore-ProcessEnvironment {
    param(
        [string]$Name,
        [AllowNull()]
        [string]$Value
    )

    if ($null -eq $Value) {
        Remove-Item "Env:$Name" -ErrorAction SilentlyContinue
    } else {
        Set-Item "Env:$Name" $Value
    }
}


$checks = [System.Collections.Generic.List[object]]::new()
$monitor = $null
$logFile = $null
$stderrFile = $null
$isReplay = -not [string]::IsNullOrWhiteSpace($LogPath)
$telemetry = $null
$backend = $null

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

    if ([string]::IsNullOrWhiteSpace($BuildDir)) {
        $BuildDir = if ($Source -eq "AD7606") {
            "build-keypad-timing-ad7606"
        } else {
            "build-keypad-timing-esp-adc"
        }
    }

    if ($isReplay) {
        $logFile = (Resolve-Path -LiteralPath $LogPath).Path
        Add-Check $checks "Live serial preflight" "SKIPPED" "Replay mode from -LogPath; no hardware access."
        Add-Check $checks "Timing-profile build" "SKIPPED" "Replay mode from -LogPath; no build or flash access."
        Add-Check $checks "Timing-profile flash" "SKIPPED" "Replay mode from -LogPath; no flash access."
    } else {
        $profileSdkconfig = [IO.Path]::GetFullPath((Join-Path $repo (Join-Path $BuildDir "sdkconfig")))
        $oldTimingProfile = [Environment]::GetEnvironmentVariable("GEOLOCATOR_KEYPAD_TIMING_PROFILE", "Process")
        $oldAdcBenchProfile = [Environment]::GetEnvironmentVariable("GEOLOCATOR_ADC_BENCH_PROFILE", "Process")
        try {
            $env:GEOLOCATOR_KEYPAD_TIMING_PROFILE = "1"
            if ($Source -eq "ESP_ADC_BENCH") {
                $env:GEOLOCATOR_ADC_BENCH_PROFILE = "1"
            } else {
                Remove-Item Env:GEOLOCATOR_ADC_BENCH_PROFILE -ErrorAction SilentlyContinue
            }

            & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $runIdf `
                -Action build -ProjectPath $ProjectPath -BuildDir $BuildDir `
                -Sdkconfig $profileSdkconfig -Jobs $Jobs
            if ($LASTEXITCODE -ne 0) {
                throw "Timing-profile build failed with exit code $LASTEXITCODE."
            }
        } finally {
            Restore-ProcessEnvironment "GEOLOCATOR_KEYPAD_TIMING_PROFILE" $oldTimingProfile
            Restore-ProcessEnvironment "GEOLOCATOR_ADC_BENCH_PROFILE" $oldAdcBenchProfile
        }
        Add-Check $checks "Timing-profile build" "PASS" ("Built isolated $Source timing profile in {0}." -f $BuildDir)

        & $checkPorts -Ports ("{0},{1}" -f $FlashPort, $LogPort)
        if ($LASTEXITCODE -ne 0) {
            throw "Live $FlashPort/$LogPort preflight failed."
        }
        Add-Check $checks "Live serial preflight" "PASS" ("Live ports available: {0}, {1}." -f $FlashPort, $LogPort)

        $hilDir = Join-Path $repo "build\hil"
        New-Item -ItemType Directory -Path $hilDir -Force | Out-Null
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $logFile = Join-Path $hilDir ("keypad-timing-{0}-{1}.log" -f $Source.ToLowerInvariant(), $stamp)
        $stderrFile = Join-Path $hilDir ("keypad-timing-{0}-{1}.err.log" -f $Source.ToLowerInvariant(), $stamp)

        $project = Resolve-RepositoryPath $ProjectPath
        $monitorArgs = @(
            "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"{0}"' -f $runIdf),
            "-Action", "monitor", "-ProjectPath", $ProjectPath,
            "-BuildDir", $BuildDir, "-Port", $LogPort
        )
        Write-Host ("Starting timing monitor on {0}; output: {1}" -f $LogPort, $logFile)
        $monitor = Start-Process -FilePath $powershell -ArgumentList $monitorArgs `
            -WorkingDirectory $project -RedirectStandardOutput $logFile `
            -RedirectStandardError $stderrFile -PassThru -WindowStyle Hidden
        Start-Sleep -Seconds 2
        if ($monitor.HasExited) {
            throw "COM6 monitor exited before timing HIL execution. See $stderrFile."
        }

        if ($SkipFlash) {
            Add-Check $checks "Timing-profile flash" "SKIPPED" "-SkipFlash supplied; existing timing-profile firmware is being observed."
        } else {
            & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $runIdf `
                -Action flash -ProjectPath $ProjectPath -BuildDir $BuildDir -Port $FlashPort
            if ($LASTEXITCODE -ne 0) {
                throw "Timing-profile flash failed with exit code $LASTEXITCODE."
            }
            Add-Check $checks "Timing-profile flash" "PASS" ("Flashed through {0}." -f $FlashPort)
        }
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $content = Read-TimingLog $logFile
        if (-not $isReplay -and $stderrFile) {
            $content = $content + "`n" + (Read-TimingLog $stderrFile)
        }
        $backend = Test-TimingBackendMarker $content $Source $ExpectedSampleRateHz
        $telemetry = Test-KeypadTimingTelemetry `
            -Content $content `
            -ExpectedSource $Source `
            -MinimumWindows $MinimumWindows `
            -MinimumSnapshots $MinimumSnapshots

        if ($backend.Pass -and $telemetry.Pass) {
            break
        }
        if ($telemetry.InvalidTelemetry) {
            break
        }
        if ($isReplay) {
            break
        }
        if ($null -ne $monitor -and $monitor.HasExited) {
            break
        }
        if ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds $PollIntervalMs
        }
    } while ((Get-Date) -lt $deadline)

    if ($null -eq $backend) {
        $backend = Test-TimingBackendMarker "" $Source $ExpectedSampleRateHz
    }
    if ($null -eq $telemetry) {
        $telemetry = Test-KeypadTimingTelemetry "" $Source $MinimumWindows $MinimumSnapshots
    }

    $missingStatus = if ($isReplay) { "FAIL" } else { "BLOCKED" }
    $telemetryStatus = if ($telemetry.InvalidTelemetry) { "FAIL" } else { $missingStatus }
    Add-Check $checks "Selected acquisition backend" `
        $(if ($backend.Pass -and $telemetry.SourcePass) { "PASS" } else { $telemetryStatus }) `
        $(if ($backend.Pass -and $telemetry.SourcePass) {
            $backend.Detail
        } else {
            "$($backend.Detail) $($telemetry.SourceDetail)"
        })
    Add-Check $checks "Keypad ADC snapshot timing" `
        $(if ($telemetry.AdcPass) { "PASS" } else { $telemetryStatus }) `
        $telemetry.AdcDetail
    Add-Check $checks "ESP_TIMER_TASK dispatch timing" `
        $(if ($telemetry.TimerPass) { "PASS" } else { $telemetryStatus }) `
        $telemetry.TimerDetail

    if ($isReplay -and $ConfirmFrequentKeypadActivity) {
        Add-Check $checks "Frequent physical keypad activity" "BLOCKED" `
            "-ConfirmFrequentKeypadActivity cannot be used with -LogPath: a captured log cannot prove physical key presses."
    } elseif ($ConfirmFrequentKeypadActivity) {
        Add-Check $checks "Frequent physical keypad activity" "PASS" `
            "Manual HIL attestation supplied: keypad was pressed frequently during the timing window."
    } elseif ($RequireFrequentKeypadActivity) {
        Add-Check $checks "Frequent physical keypad activity" "BLOCKED" `
            "Press the keypad repeatedly during the timing window, then rerun with -ConfirmFrequentKeypadActivity."
    } else {
        Add-Check $checks "Frequent physical keypad activity" "SKIPPED" `
            "Timing telemetry is collected, but physical key activity remains a manual HIL condition; use -RequireFrequentKeypadActivity to gate it."
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

Write-Host "===== Keypad ADC timing HIL report ====="
Write-Host ("{0,-42} {1,-9} {2}" -f "CHECK", "STATUS", "DETAIL")
Write-Host ("{0,-42} {1,-9} {2}" -f "-----", "------", "------")
foreach ($check in $checks) {
    Write-Host ("{0,-42} {1,-9} {2}" -f $check.Name, $check.Status, $check.Detail)
}
if ($isReplay) {
    Write-Host ("LOG_REPLAY: {0}" -f $logFile)
}
Write-Host "LIMITATIONS: The opt-in probe adds diagnostic load. A valid p95 is a bounded-histogram upper bound; when its rank lands in the overflow bucket, p95 is censored and reported with p95_overflow=1 and p95_censored_above_us. Repeat separate live windows for AD7606 idle, AD7606 with frequent keypad presses, and ESP_ADC_BENCH at 500Hz with frequent keypad presses; serial replay cannot prove physical activity or board timing."
Write-Host ("KEYPAD_TIMING_TELEMETRY_RESULT: {0}" -f $result)
Write-Host "PERFORMANCE_ACCEPTANCE: NOT_ESTABLISHED"

if ($result -eq "PASS") {
    exit 0
}
exit 1
