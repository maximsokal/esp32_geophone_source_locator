[CmdletBinding()]
param()

# Mock-only self-test for monitor_esp32_logs.ps1: fake COM inventory and fake runner process.
# No physical serial port or ESP32 is touched. ASCII-only file; non-ASCII text is built from char codes.

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "monitor_esp32_logs.ps1") -LibraryOnly

$script:Passes = 0
$script:Failures = 0

function Assert-Check {
    param([bool]$Condition, [string]$Name, [string]$Detail = "")
    if ($Condition) {
        $script:Passes++
        Write-Host "PASS $Name"
    } else {
        $script:Failures++
        Write-Host "FAIL $Name $Detail"
    }
}

$cyr = -join ([char[]](0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442))
$mark = [string][char]0x2713
$root = Join-Path ([IO.Path]::GetTempPath()) ("monitor-selftest-" + [guid]::NewGuid().ToString("N"))
$null = New-Item -ItemType Directory -Path $root

try {
    # Fake runner stands in for run_idf.ps1: it receives the same arguments and prints monitor-like output.
    $fakeRunner = Join-Path $root "fake_run_idf.ps1"
    $runnerBody = @'
param([string]$Action, [string]$ProjectPath, [string]$BuildDir, [string]$Port,
      [string]$Mode = "normal", [int]$Lines = 0, [string]$StateFile = "")
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$attempt = 1
if ($StateFile) {
    if (Test-Path -LiteralPath $StateFile) { $attempt = 1 + [int](Get-Content -LiteralPath $StateFile) }
    Set-Content -LiteralPath $StateFile -Value $attempt -Encoding ASCII
}
$cyr = -join ([char[]](0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442))
$mark = [string][char]0x2713
$banner = "--- esp-idf-monitor 1.9.0 on $Port 115200 ---"
switch ($Mode) {
    "normal" {
        Write-Output $banner
        Write-Output "Executing action: monitor"
        Write-Output "Running idf_monitor in directory '$ProjectPath'"
        Write-Output 'Executing "python idf_monitor.py"...'
        for ($i = 1; $i -le $Lines; $i++) { Write-Output ("LINE-{0:D6} {1} {2}" -f $i, $cyr, $mark) }
        exit 0
    }
    "fail" {
        Write-Output $banner
        Write-Output "--- Error: ELF file not found."
        exit 3
    }
    { $_ -in @("retry_zero", "retry_nonzero", "retry_missing") } {
        if ($attempt -le 2) {
            if ($Mode -eq "retry_missing") {
                [Console]::Error.WriteLine("Required live serial port(s) missing: $Port")
            } else {
                [Console]::Error.WriteLine($banner)
                Write-Output "Executing action: monitor"
                Write-Output "Running idf_monitor in directory '$ProjectPath'"
                Write-Output 'Executing "python idf_monitor.py"...'
                [Console]::Error.WriteLine("--- Connection to \\.\$Port failed. Available ports:")
                Write-Output "Cannot configure port, something went wrong. Original message: PermissionError(13, 'A device attached to the system is not functioning.', None, 31)"
            }
            if ($Mode -eq "retry_zero") { exit 0 }
            exit 3
        }
        Write-Output $banner
        Write-Output ("LINE-000001 {0}" -f $cyr)
        exit 0
    }
    "reconnect" {
        Write-Output $banner
        Write-Output ("LINE-000001 {0}" -f $cyr)
        Start-Sleep -Milliseconds 150
        [Console]::Error.WriteLine("--- Waiting for the device to reconnect")
        [Console]::Error.WriteLine("Cannot configure port, something went wrong.")
        Start-Sleep -Seconds 2
        Write-Output ("LINE-000002 {0}" -f $cyr)
        exit 0
    }
    "silent" {
        Start-Sleep -Seconds 2
        exit 0
    }
}
'@
    [IO.File]::WriteAllText($fakeRunner, $runnerBody, [Text.UTF8Encoding]::new($false))

    function Invoke-FakeSession {
        param(
            [string]$Mode,
            [int]$Lines = 0,
            [string]$StateFile = "",
            [double]$NoDataWarningSeconds = 5
        )
        $runnerArgs = @("-Mode", $Mode, "-Lines", "$Lines")
        if ($StateFile) { $runnerArgs += @("-StateFile", $StateFile) }
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $session = Invoke-MonitorSession -Port "COM3" -ProjectRoot $root -RunnerPath $fakeRunner `
            -LogDirectory (Join-Path $root "logs\esp32") -RunnerArguments $runnerArgs `
            -NoDataWarningSeconds $NoDataWarningSeconds
        $clock.Stop()
        return [pscustomobject]@{ Session = $session; Seconds = $clock.Elapsed.TotalSeconds }
    }

    # 1. Port inventory: sorting, Espressif detection by VID 303A, unknown port fallback.
    $details = @(
        [pscustomobject]@{ DeviceID = "COM3"; Name = "USB Serial Device (COM3)"; Manufacturer = "Espressif"; PNPDeviceID = "USB\VID_303A&PID_1001\AABB" },
        [pscustomobject]@{ DeviceID = "COM4"; Name = "USB Serial Device (COM4)"; Manufacturer = ""; PNPDeviceID = "USB\VID_1A86&PID_7523\X1" }
    )
    $inventory = @(Get-PortInventory -PortNames @("COM10", "COM4", "COM3") -DeviceDetails $details)
    Assert-Check ($inventory.Count -eq 3) "inventory lists all ports" "count=$($inventory.Count)"
    Assert-Check (($inventory | ForEach-Object { $_.Port }) -join "," -eq "COM3,COM4,COM10") "inventory sorts COM numbers numerically"
    Assert-Check ($inventory[0].IsEspressif -and $inventory[0].VID -eq "303A" -and $inventory[0].PID -eq "1001") "Espressif VID/PID detected on COM3"
    Assert-Check (-not $inventory[1].IsEspressif -and $inventory[1].VID -eq "1A86") "non-Espressif port not flagged"
    Assert-Check ($inventory[2].Name -eq "Serial port" -and -not $inventory[2].IsEspressif) "port without details gets fallback name"

    # 2. Picker choice resolution.
    $one = @($inventory[0])
    Assert-Check ((Resolve-PortChoice -Choice "" -Inventory $one).Port -eq "COM3") "Enter selects the only port"
    Assert-Check ((Resolve-PortChoice -Choice "" -Inventory $inventory).Action -eq "Invalid") "Enter with several ports is invalid"
    Assert-Check ((Resolve-PortChoice -Choice "2" -Inventory $inventory).Port -eq "COM4") "number selects listed port"
    Assert-Check ((Resolve-PortChoice -Choice "4" -Inventory $inventory).Action -eq "Invalid") "out-of-range number is invalid"
    Assert-Check ((Resolve-PortChoice -Choice "m" -Inventory $inventory).Action -eq "Manual") "M requests manual entry"
    Assert-Check ((Resolve-PortChoice -Choice "R" -Inventory $inventory).Action -eq "Refresh") "R refreshes"
    Assert-Check ((Resolve-PortChoice -Choice "q" -Inventory $inventory).Action -eq "Cancel") "Q cancels"
    Assert-Check ((Resolve-PortChoice -Choice "x" -Inventory $inventory).Action -eq "Invalid") "garbage input is invalid"

    # 3. Manual COM entry validation.
    Assert-Check ((ConvertTo-ManualPort "COM12") -eq "COM12") "manual COM12 accepted"
    Assert-Check ((ConvertTo-ManualPort " com3 ") -eq "COM3") "manual input trimmed and upper-cased"
    Assert-Check ($null -eq (ConvertTo-ManualPort "COM0")) "COM0 rejected"
    Assert-Check ($null -eq (ConvertTo-ManualPort "3")) "bare number rejected"
    Assert-Check ($null -eq (ConvertTo-ManualPort "COM3x")) "trailing junk rejected"

    # 4. Unique log names, UTF-8 without BOM, no overwrite.
    $logDir = Join-Path $root "names"
    $start = [datetime]::new(2026, 10, 25, 19, 35, 0)
    $first = Open-UniqueMonitorLog -LogDirectory $logDir -Port "COM3" -StartedAt $start
    $first.Writer.WriteLine("first " + $cyr)
    $first.Writer.Dispose()
    $second = Open-UniqueMonitorLog -LogDirectory $logDir -Port "COM3" -StartedAt $start
    $second.Writer.Dispose()
    Assert-Check ([IO.Path]::GetFileName($first.Path) -eq "esp32_COM3_2026-10-25_19-35-00.log") "first log name uses port and timestamp" $first.Path
    Assert-Check ([IO.Path]::GetFileName($second.Path) -eq "esp32_COM3_2026-10-25_19-35-00_01.log") "second log in same second gets suffix" $second.Path
    $firstBytes = [IO.File]::ReadAllBytes($first.Path)
    Assert-Check (-not ($firstBytes.Length -ge 3 -and $firstBytes[0] -eq 0xEF -and $firstBytes[1] -eq 0xBB -and $firstBytes[2] -eq 0xBF)) "log has no BOM"
    Assert-Check ([Text.Encoding]::UTF8.GetString($firstBytes).Trim() -eq ("first " + $cyr)) "log keeps UTF-8 Cyrillic text"

    # 5. Normal run: high-volume output is complete, ordered, and RUNNING appears after device data.
    $run = Invoke-FakeSession -Mode "normal" -Lines 2000
    $session = $run.Session
    Assert-Check ($session.Outcome -eq "Exited" -and $session.ExitCode -eq 0) "normal run exits cleanly" "outcome=$($session.Outcome) exit=$($session.ExitCode)"
    Assert-Check ($session.Running) "RUNNING is reported after device output"
    Assert-Check ($run.Seconds -lt 30) "2000 lines drain without per-line throttling" ("elapsed={0:N1}s" -f $run.Seconds)
    $logLines = [IO.File]::ReadAllLines($session.LogPath, [Text.Encoding]::UTF8)
    $serial = @($logLines | Where-Object { $_ -match '^LINE-\d{6} ' })
    Assert-Check ($serial.Count -eq 2000) "no serial lines lost" "count=$($serial.Count)"
    $ordered = $serial.Count -eq 2000
    for ($i = 0; $ordered -and $i -lt $serial.Count; $i++) {
        if (-not $serial[$i].StartsWith(("LINE-{0:D6} " -f ($i + 1)))) { $ordered = $false }
    }
    Assert-Check $ordered "serial lines stay in order"
    Assert-Check ($serial.Count -gt 0 -and $serial[0].Contains($cyr) -and $serial[0].Contains($mark)) "UTF-8 and symbols survive the monitor pipe"
    $bannerIndex = [array]::IndexOf($logLines, ($logLines | Where-Object { $_ -like "--- esp-idf-monitor*" } | Select-Object -First 1))
    $runningIndex = [array]::IndexOf($logLines, "# ESP-IDF Monitor: RUNNING")
    Assert-Check ($bannerIndex -ge 0 -and $runningIndex -gt $bannerIndex) "RUNNING note is written after the banner"
    $firstDataIndex = [array]::IndexOf($logLines, $serial[0])
    Assert-Check ($runningIndex -gt $firstDataIndex) "launcher messages never report RUNNING before UART data"
    Assert-Check ((Split-Path -Leaf $session.LogPath) -match '^esp32_COM3_\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}(_\d{2})?\.log$') "session log is saved under logs/esp32"

    # 6. Monitor fails: error is logged, outcome is Failed, no hang.
    $failed = Invoke-FakeSession -Mode "fail"
    Assert-Check ($failed.Session.Outcome -eq "Failed" -and $failed.Session.ExitCode -eq 3) "failed monitor reports Failed with exit code" "outcome=$($failed.Session.Outcome)"
    Assert-Check (-not $failed.Session.Running) "failed monitor never reports RUNNING"

    # 7. Reproduce the real initial COM error, including ESP-IDF's successful process exit.
    foreach ($mode in @("retry_zero", "retry_nonzero", "retry_missing")) {
        $stateFile = Join-Path $root "$mode.attempts"
        $logsBefore = @(Get-ChildItem -LiteralPath (Join-Path $root 'logs/esp32') -Filter '*.log').Count
        $retry = Invoke-FakeSession -Mode $mode -StateFile $stateFile
        $retryLines = [IO.File]::ReadAllLines($retry.Session.LogPath, [Text.Encoding]::UTF8)
        $logsAfter = @(Get-ChildItem -LiteralPath (Join-Path $root 'logs/esp32') -Filter '*.log').Count
        Assert-Check ([int](Get-Content -LiteralPath $stateFile) -eq 3) "$mode waits through two failed opens"
        Assert-Check ($retry.Session.Outcome -eq "Exited" -and $retry.Session.Running) "$mode records UART after recovery"
        Assert-Check ($logsAfter -eq $logsBefore + 1 -and
            @($retryLines | Where-Object { $_ -like '# Started *' }).Count -eq 1 -and
            @($retryLines | Where-Object { $_ -match 'Cannot configure port|Required live serial port\(s\) missing' }).Count -eq 2) "$mode keeps errors and recovered data in one log"
        Assert-Check (@($retryLines | Where-Object { $_ -eq '# ESP-IDF Monitor: RUNNING' }).Count -eq 1) "$mode has no false RUNNING on failure"
        $dataIndex = [array]::FindIndex($retryLines, [Predicate[string]]{ param($line) $line -like 'LINE-*' })
        $retryRunningIndex = [array]::IndexOf($retryLines, '# ESP-IDF Monitor: RUNNING')
        Assert-Check ($dataIndex -ge 0 -and $retryRunningIndex -gt $dataIndex) "$mode starts RUNNING only after UART data"
        Assert-Check (($retryLines -join "`n") -match 'retrying') "$mode logs that it is waiting and retrying"
    }

    # 8. Let ESP-IDF's native reconnect complete instead of killing its reader process.
    $stateFile = Join-Path $root "reconnect.attempts"
    $reconnect = Invoke-FakeSession -Mode "reconnect" -StateFile $stateFile
    $reconnectLog = [IO.File]::ReadAllText($reconnect.Session.LogPath, [Text.Encoding]::UTF8)
    Assert-Check ($reconnect.Session.Outcome -eq "Exited") "native reconnect completes normally"
    Assert-Check ([int](Get-Content -LiteralPath $stateFile) -eq 1) "native reconnect uses the same monitor process"
    Assert-Check ($reconnectLog -match 'LINE-000001' -and $reconnectLog -match 'LINE-000002') "UART before and after reconnect stays in one log"

    # 9. No UART data: the monitor stays active and the log says so.
    $quiet = Invoke-FakeSession -Mode "silent" -NoDataWarningSeconds 1
    Assert-Check ($quiet.Session.Outcome -eq "Exited" -and -not $quiet.Session.Running) "silent monitor is not reported as RUNNING"
    $quietLog = [IO.File]::ReadAllText($quiet.Session.LogPath, [Text.Encoding]::UTF8)
    Assert-Check ($quietLog -match "WAITING: no UART data arriving") "no-data notice is logged"
    $metadataOnly = Invoke-FakeSession -Mode "normal" -Lines 0
    $metadataLog = [IO.File]::ReadAllText($metadataOnly.Session.LogPath, [Text.Encoding]::UTF8)
    Assert-Check (-not $metadataOnly.Session.Running) "launcher-only capture never reports UART data"
    Assert-Check ($metadataLog -match 'without receiving UART data' -and $metadataLog -notmatch 'UART log saved:') "empty capture is not presented as a saved UART log"

    # 10. Invalid port is rejected before any process starts.
    $rejected = $false
    try { Invoke-MonitorSession -Port "bad" -ProjectRoot $root -RunnerPath $fakeRunner } catch { $rejected = $true }
    Assert-Check $rejected "invalid COM port throws before starting"
}
finally {
    $tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not ([IO.Path]::GetFullPath($root)).StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $root) -notlike 'monitor-selftest-*') { throw "Unsafe self-test cleanup path: $root" }
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Passed: $script:Passes  Failed: $script:Failures"
if ($script:Failures -gt 0) { exit 1 }
exit 0
