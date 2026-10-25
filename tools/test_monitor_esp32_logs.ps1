[CmdletBinding()]
param()

# Mock-only self-test for monitor_esp32_logs.ps1: fake COM inventory, fake runner process, fake port presence.
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
      [string]$Mode = "normal", [int]$Lines = 0, [string]$PidFile = "")
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
if ($PidFile) { Set-Content -LiteralPath $PidFile -Value $PID -Encoding ASCII }
$cyr = -join ([char[]](0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442))
$mark = [string][char]0x2713
$banner = "--- esp-idf-monitor 1.9.0 on $Port 115200 ---"
switch ($Mode) {
    "normal" {
        Write-Output $banner
        for ($i = 1; $i -le $Lines; $i++) { Write-Output ("LINE-{0:D6} {1} {2}" -f $i, $cyr, $mark) }
        exit 0
    }
    "fail" {
        Write-Output $banner
        Write-Output "Connection to $Port failed: PermissionError(13, 'Access is denied')"
        exit 3
    }
    "hang" {
        Write-Output $banner
        Write-Output ("LINE-000001 {0}" -f $cyr)
        Write-Output "Waiting for the device to reconnect"
        Start-Sleep -Seconds 300
        exit 0
    }
    "hang_quiet" {
        Write-Output $banner
        Write-Output ("LINE-000001 {0}" -f $cyr)
        Start-Sleep -Seconds 300
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
            [string]$PidFile = "",
            [scriptblock]$PortNameProvider = { @("COM3") },
            [double]$NoDataWarningSeconds = 5
        )
        $runnerArgs = @("-Mode", $Mode, "-Lines", "$Lines")
        if ($PidFile) { $runnerArgs += @("-PidFile", $PidFile) }
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $session = Invoke-MonitorSession -Port "COM3" -ProjectRoot $root -RunnerPath $fakeRunner `
            -LogDirectory (Join-Path $root "logs\esp32") -RunnerArguments $runnerArgs `
            -PortNameProvider $PortNameProvider -NoDataWarningSeconds $NoDataWarningSeconds
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
    Assert-Check ((Split-Path -Leaf $session.LogPath) -match '^esp32_COM3_\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}(_\d{2})?\.log$') "session log is saved under logs/esp32"

    # 6. Monitor fails: error is logged, outcome is Failed, no hang.
    $failed = Invoke-FakeSession -Mode "fail"
    Assert-Check ($failed.Session.Outcome -eq "Failed" -and $failed.Session.ExitCode -eq 3) "failed monitor reports Failed with exit code" "outcome=$($failed.Session.Outcome)"
    Assert-Check (-not $failed.Session.Running) "failed monitor never reports RUNNING"

    # 7. Device reconnect text: stop only this monitor's tree and report Disconnected.
    $pidFile = Join-Path $root "hang.pid"
    $hang = Invoke-FakeSession -Mode "hang" -PidFile $pidFile
    Assert-Check ($hang.Session.Outcome -eq "Disconnected" -and $hang.Session.Disconnected) "reconnect wait ends as Disconnected" "outcome=$($hang.Session.Outcome)"
    Assert-Check ($hang.Seconds -lt 30) "disconnect stops the monitor promptly" ("elapsed={0:N1}s" -f $hang.Seconds)
    $childPid = [int]([IO.File]::ReadAllText($pidFile).Trim())
    Assert-Check ($null -eq (Get-Process -Id $childPid -ErrorAction SilentlyContinue)) "monitor process tree is gone after disconnect"

    # 8. Port disappears silently (no reconnect text): detected through port presence and treated as Disconnected.
    $script:portCalls = 0
    $vanish = { $script:portCalls++; if ($script:portCalls -le 3) { @("COM3") } else { @() } }
    $silent = Invoke-FakeSession -Mode "hang_quiet" -PortNameProvider $vanish
    Assert-Check ($silent.Session.Outcome -eq "Disconnected") "vanished port ends as Disconnected" "outcome=$($silent.Session.Outcome)"
    Assert-Check ($silent.Seconds -lt 30) "vanished port stops the monitor promptly" ("elapsed={0:N1}s" -f $silent.Seconds)

    # 9. No UART data: the monitor stays active and the log says so.
    $quiet = Invoke-FakeSession -Mode "silent" -NoDataWarningSeconds 1
    Assert-Check ($quiet.Session.Outcome -eq "Exited" -and -not $quiet.Session.Running) "silent monitor is not reported as RUNNING"
    $quietLog = [IO.File]::ReadAllText($quiet.Session.LogPath, [Text.Encoding]::UTF8)
    Assert-Check ($quietLog -match "CONNECTING: no UART data yet") "no-data notice is logged"

    # 10. Invalid port is rejected before any process starts.
    $rejected = $false
    try { Invoke-MonitorSession -Port "bad" -ProjectRoot $root -RunnerPath $fakeRunner } catch { $rejected = $true }
    Assert-Check $rejected "invalid COM port throws before starting"
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Passed: $script:Passes  Failed: $script:Failures"
if ($script:Failures -gt 0) { exit 1 }
exit 0
