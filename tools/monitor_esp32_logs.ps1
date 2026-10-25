[CmdletBinding()]
param(
    [switch]$LibraryOnly
)

$ErrorActionPreference = "Stop"

function Get-SerialPortNames {
    $names = [System.IO.Ports.SerialPort]::GetPortNames() |
        ForEach-Object { $_.Trim().ToUpperInvariant() } |
        Where-Object { $_ -match '^COM[1-9]\d*$' }
    return @($names | Sort-Object { [int]($_.Substring(3)) } -Unique)
}

function Get-PortInventory {
    [CmdletBinding()]
    param(
        [string[]]$PortNames,
        [object[]]$DeviceDetails
    )

    if (-not $PSBoundParameters.ContainsKey("PortNames")) {
        $PortNames = @(Get-SerialPortNames)
    }
    if (-not $PSBoundParameters.ContainsKey("DeviceDetails")) {
        $DeviceDetails = @()
        try { $DeviceDetails += @(Get-CimInstance Win32_SerialPort -ErrorAction Stop) } catch {}
        try { $DeviceDetails += @(Get-PnpDevice -Class Ports -ErrorAction Stop) } catch {}
    }

    foreach ($port in @($PortNames | ForEach-Object { $_.Trim().ToUpperInvariant() } |
            Where-Object { $_ -match '^COM[1-9]\d*$' } |
            Sort-Object { [int]($_.Substring(3)) } -Unique)) {
        $details = @($DeviceDetails | Where-Object {
            $candidate = @($_.DeviceID, $_.Name, $_.FriendlyName, $_.Description) -join " "
            $candidate -match "(?i)(^|\W)$port(\W|$)"
        })
        $displayName = @($details | ForEach-Object {
            if ($_.FriendlyName) { $_.FriendlyName }
            elseif ($_.Name) { $_.Name }
            elseif ($_.Description) { $_.Description }
        } | Where-Object { $_ } | Select-Object -First 1)[0]
        $manufacturer = @($details | ForEach-Object { $_.Manufacturer } |
            Where-Object { $_ } | Select-Object -First 1)[0]
        $deviceIds = @($details | ForEach-Object { "$($_.PNPDeviceID) $($_.InstanceId)" }) -join " "
        $vidPid = [regex]::Match($deviceIds, '(?i)VID_([0-9A-F]{4})&PID_([0-9A-F]{4})')
        $vid = if ($vidPid.Success) { $vidPid.Groups[1].Value.ToUpperInvariant() } else { "" }
        $productId = if ($vidPid.Success) { $vidPid.Groups[2].Value.ToUpperInvariant() } else { "" }
        $isEspressif = $vid -eq "303A" -or
            (($details | ForEach-Object { "$($_.Name) $($_.FriendlyName) $($_.Manufacturer)" }) -join " ") -match '(?i)Espressif'
        if (-not $manufacturer -and $vid -eq "303A") { $manufacturer = "Espressif" }

        [pscustomobject]@{
            Port         = $port
            Name         = if ($displayName) { $displayName } else { "Serial port" }
            Manufacturer = if ($manufacturer) { $manufacturer } else { "" }
            VID          = $vid
            PID          = $productId
            IsEspressif  = [bool]$isEspressif
        }
    }
}

function Resolve-PortChoice {
    param(
        [string]$Choice,
        [object[]]$Inventory
    )

    $value = if ($null -eq $Choice) { "" } else { $Choice.Trim().ToUpperInvariant() }
    if ($value -eq "Q") { return [pscustomobject]@{ Action = "Cancel"; Port = $null } }
    if ($value -eq "R") { return [pscustomobject]@{ Action = "Refresh"; Port = $null } }
    if ($value -eq "M") { return [pscustomobject]@{ Action = "Manual"; Port = $null } }
    if (-not $value -and @($Inventory).Count -eq 1) {
        return [pscustomobject]@{ Action = "Select"; Port = $Inventory[0].Port }
    }
    if ($value -match '^\d+$') {
        $index = [int]$value
        if ($index -ge 1 -and $index -le @($Inventory).Count) {
            return [pscustomobject]@{ Action = "Select"; Port = $Inventory[$index - 1].Port }
        }
    }
    return [pscustomobject]@{ Action = "Invalid"; Port = $null }
}

function ConvertTo-ManualPort {
    param([string]$Value)
    $port = if ($null -eq $Value) { "" } else { $Value.Trim().ToUpperInvariant() }
    if ($port -match '^COM[1-9]\d*$') { return $port }
    return $null
}

function Show-PortPicker {
    while ($true) {
        $inventory = @(Get-PortInventory)
        Write-Host ""
        Write-Host "ESP-IDF SERIAL MONITOR"
        if ($inventory.Count -eq 0) {
            Write-Host "No COM ports detected."
        } else {
            Write-Host "Detected serial ports:"
            for ($i = 0; $i -lt $inventory.Count; $i++) {
                $item = $inventory[$i]
                $metadata = @($item.Name, $item.Manufacturer, $(if ($item.VID) { "VID_$($item.VID)&PID_$($item.PID)" })) |
                    Where-Object { $_ } | Select-Object -Unique
                $marker = if ($item.IsEspressif) { " [Espressif]" } else { "" }
                Write-Host ("[{0}] {1} - {2}{3}" -f ($i + 1), $item.Port, ($metadata -join ", "), $marker)
            }
        }

        $default = if ($inventory.Count -eq 1) { " (Enter selects $($inventory[0].Port))" } else { "" }
        $choice = Read-Host "Select port [number, M manual, R refresh, Q cancel]$default"
        $resolved = Resolve-PortChoice -Choice $choice -Inventory $inventory
        switch ($resolved.Action) {
            "Select" { return $resolved.Port }
            "Cancel" { return $null }
            "Refresh" { continue }
            "Manual" {
                $manual = ConvertTo-ManualPort (Read-Host "Enter COM port (for example COM12)")
                if ($manual) { return $manual }
                Write-Host "Invalid port. Enter COM followed by a positive number."
            }
            default { Write-Host "Invalid selection. Choose a listed number, M, R, or Q." }
        }
    }
}

function Open-UniqueMonitorLog {
    param(
        [Parameter(Mandatory = $true)][string]$LogDirectory,
        [Parameter(Mandatory = $true)][string]$Port,
        [datetime]$StartedAt = (Get-Date)
    )

    $null = New-Item -ItemType Directory -Path $LogDirectory -Force
    $stamp = $StartedAt.ToString("yyyy-MM-dd_HH-mm-ss", [Globalization.CultureInfo]::InvariantCulture)
    for ($suffix = 0; $suffix -lt 1000; $suffix++) {
        $extra = if ($suffix -eq 0) { "" } else { "_{0:D2}" -f $suffix }
        $path = Join-Path $LogDirectory ("esp32_{0}_{1}{2}.log" -f $Port, $stamp, $extra)
        try {
            $stream = [IO.FileStream]::new(
                $path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite
            )
            $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
            return [pscustomobject]@{ Path = $path; Writer = $writer }
        } catch [IO.IOException] {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw }
        }
    }
    throw "Could not allocate a unique monitor log name in $LogDirectory."
}

function New-MonitorStartInfo {
    param(
        [Parameter(Mandatory = $true)][string]$RunnerPath,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$Port,
        [string[]]$RunnerArguments = @()
    )

    $powershellPath = Join-Path $PSHOME "powershell.exe"
    $arguments = @(
        "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $RunnerPath,
        "-Action", "monitor", "-ProjectPath", $ProjectRoot, "-BuildDir", "build", "-Port", $Port
    ) + $RunnerArguments
    $quoted = foreach ($argument in $arguments) {
        '"' + ([string]$argument).Replace('"', '\"') + '"'
    }
    $utf8 = [Text.UTF8Encoding]::new($false)
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $powershellPath
    $info.Arguments = $quoted -join " "
    $info.WorkingDirectory = $ProjectRoot
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $false
    $info.RedirectStandardInput = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $utf8
    $info.StandardErrorEncoding = $utf8
    $info.EnvironmentVariables["PYTHONUTF8"] = "1"
    $info.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8"
    return $info
}

function Remove-AnsiSequences {
    param([string]$Text)
    return [regex]::Replace($Text, '\x1B\[[0-?]*[ -/]*[@-~]', '')
}

function Test-MonitorBanner {
    param([string]$Text)
    return (Remove-AnsiSequences $Text) -match '(?i)^---\s*esp-idf-monitor\b'
}

function Test-SerialOutputLine {
    param([string]$Text)
    $clean = (Remove-AnsiSequences $Text).Trim()
    if (-not $clean -or $clean.StartsWith("---")) { return $false }
    if ($clean -match '(?i)Waiting for the device to reconnect|Connection to COM\d+ failed|Available ports:') {
        return $false
    }
    return $true
}

function Write-MonitorNote {
    param(
        [IO.StreamWriter]$Writer,
        [string]$Message,
        [switch]$ShowOutput
    )
    $line = "# $Message"
    $Writer.WriteLine($line)
    $Writer.Flush()
    if ($ShowOutput) { Write-Host $Message }
}

function Write-MonitorOutputLine {
    param(
        [IO.StreamWriter]$Writer,
        [string]$Line,
        [switch]$ShowOutput
    )
    $Writer.WriteLine($Line)
    $Writer.Flush()
    if ($ShowOutput) { [Console]::Out.WriteLine($Line) }
}

function Stop-MonitorProcessTree {
    param([Diagnostics.Process]$Process)
    if ($null -eq $Process -or $Process.HasExited) { return }
    $taskkill = Join-Path $env:SystemRoot "System32\taskkill.exe"
    if (Test-Path -LiteralPath $taskkill -PathType Leaf) {
        & $taskkill /PID $Process.Id /T /F *> $null
    }
    if (-not $Process.HasExited) {
        try { $Process.Kill() } catch {}
    }
    try { $null = $Process.WaitForExit(5000) } catch {}
}

function Invoke-MonitorSession {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Port,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$RunnerPath,
        [string]$LogDirectory,
        [string[]]$RunnerArguments = @(),
        [scriptblock]$PortNameProvider = { Get-SerialPortNames },
        [ValidateRange(0, 300)][double]$NoDataWarningSeconds = 5,
        [switch]$ShowOutput
    )

    $selectedPort = ConvertTo-ManualPort $Port
    if (-not $selectedPort) { throw "Invalid COM port: $Port" }
    if (-not $LogDirectory) { $LogDirectory = Join-Path $ProjectRoot "logs\esp32" }
    $startedAt = Get-Date
    $log = Open-UniqueMonitorLog -LogDirectory $LogDirectory -Port $selectedPort -StartedAt $startedAt
    $relativeLog = Join-Path "logs/esp32" ([IO.Path]::GetFileName($log.Path))
    $process = $null
    $outcome = "Failed"
    $exitCode = $null
    $running = $false
    $disconnected = $false

    try {
        Write-MonitorNote $log.Writer ("Started {0:yyyy-MM-dd HH:mm:ss} | Port: {1}" -f $startedAt, $selectedPort) -ShowOutput:$ShowOutput
        Write-MonitorNote $log.Writer "ESP-IDF Monitor: CONNECTING" -ShowOutput:$ShowOutput
        Write-MonitorNote $log.Writer "Logging: ENABLED | File: $relativeLog" -ShowOutput:$ShowOutput

        $wasPresent = $false
        try { $wasPresent = @(& $PortNameProvider) -contains $selectedPort } catch {}
        $startInfo = New-MonitorStartInfo -RunnerPath $RunnerPath -ProjectRoot $ProjectRoot -Port $selectedPort -RunnerArguments $RunnerArguments
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        if (-not $process.Start()) { throw "Could not start the ESP-IDF monitor process." }
        $startedClock = [Diagnostics.Stopwatch]::StartNew()
        $stdoutTask = $process.StandardOutput.ReadLineAsync()
        $stderrTask = $process.StandardError.ReadLineAsync()
        $stdoutDone = $false
        $stderrDone = $false
        $monitorBannerSeen = $false
        $noDataNoticeShown = $false
        $lastPortCheck = [Diagnostics.Stopwatch]::StartNew()
        $missingPortChecks = 0

        while (-not ($process.HasExited -and $stdoutDone -and $stderrDone)) {
            $readAny = $false
            foreach ($streamName in @("stdout", "stderr")) {
                # Drain every read that has already completed so a burst of boot output is not throttled to one line per tick.
                while ($true) {
                    $task = if ($streamName -eq "stdout") { $stdoutTask } else { $stderrTask }
                    $done = if ($streamName -eq "stdout") { $stdoutDone } else { $stderrDone }
                    if ($done -or -not $task.IsCompleted) { break }
                    $readAny = $true
                    $line = $null
                    try { $line = $task.GetAwaiter().GetResult() }
                    catch { Write-MonitorNote $log.Writer "[monitor output capture error] $($_.Exception.Message)" -ShowOutput:$ShowOutput }
                    if ($null -eq $line) {
                        if ($streamName -eq "stdout") { $stdoutDone = $true } else { $stderrDone = $true }
                        break
                    }
                    Write-MonitorOutputLine $log.Writer $line -ShowOutput:$ShowOutput
                    if (Test-MonitorBanner $line) { $monitorBannerSeen = $true }
                    $clean = Remove-AnsiSequences $line
                    if ($clean -match 'Waiting for the device to reconnect') { $disconnected = $true }
                    if ($streamName -eq "stdout" -and $monitorBannerSeen -and
                        -not $running -and (Test-SerialOutputLine $clean)) {
                        $running = $true
                        Write-MonitorNote $log.Writer "ESP-IDF Monitor: RUNNING" -ShowOutput:$ShowOutput
                        Write-MonitorNote $log.Writer "Target: ESP32-S3 | Port: $selectedPort | Baudrate: 115200" -ShowOutput:$ShowOutput
                        Write-MonitorNote $log.Writer "Logging: ENABLED | File: $relativeLog" -ShowOutput:$ShowOutput
                        Write-MonitorNote $log.Writer "Press Ctrl+] to stop." -ShowOutput:$ShowOutput
                    }
                    if ($streamName -eq "stdout") {
                        $stdoutTask = $process.StandardOutput.ReadLineAsync()
                    } else {
                        $stderrTask = $process.StandardError.ReadLineAsync()
                    }
                }
            }

            if ($disconnected -and -not $process.HasExited) {
                Write-MonitorNote $log.Writer "USB disconnect detected; stopping this monitor process tree." -ShowOutput:$ShowOutput
                Stop-MonitorProcessTree $process
                $disconnected = $true
            }

            if ($wasPresent -and -not $disconnected -and $lastPortCheck.ElapsedMilliseconds -ge 500) {
                try {
                    $currentlyPresent = @(& $PortNameProvider) -contains $selectedPort
                    $missingPortChecks = if ($currentlyPresent) { 0 } else { $missingPortChecks + 1 }
                    if ($missingPortChecks -ge 2) {
                        $disconnected = $true
                        Write-MonitorNote $log.Writer "USB disconnect detected; stopping this monitor process tree." -ShowOutput:$ShowOutput
                        Stop-MonitorProcessTree $process
                    }
                } catch {}
                $lastPortCheck.Restart()
            }

            if (-not $running -and -not $noDataNoticeShown -and
                $NoDataWarningSeconds -gt 0 -and $startedClock.Elapsed.TotalSeconds -ge $NoDataWarningSeconds) {
                $noDataNoticeShown = $true
                Write-MonitorNote $log.Writer "CONNECTING: no UART data yet; monitor remains active." -ShowOutput:$ShowOutput
            }
            if (-not $readAny -and -not ($process.HasExited -and $stdoutDone -and $stderrDone)) {
                Start-Sleep -Milliseconds 40
            }
        }
        $process.WaitForExit()
        $exitCode = $process.ExitCode
        $outcome = if ($disconnected) { "Disconnected" } elseif ($exitCode -eq 0) { "Exited" } else { "Failed" }
        if ($exitCode -ne 0 -and -not $disconnected) {
            Write-MonitorNote $log.Writer "Monitor process exited with code $exitCode. Choose another port or retry from the picker." -ShowOutput:$ShowOutput
        } elseif (-not $disconnected) {
            Write-MonitorNote $log.Writer "Monitor stopped. Log saved: $relativeLog" -ShowOutput:$ShowOutput
        }
    } catch {
        $message = $_.Exception.Message
        try { Write-MonitorNote $log.Writer "Monitor startup/capture failed: $message" -ShowOutput:$ShowOutput } catch {}
        $outcome = "Failed"
    } finally {
        if ($process -and -not $process.HasExited) { Stop-MonitorProcessTree $process }
        if ($process) { $process.Dispose() }
        if ($log.Writer) {
            $log.Writer.Flush()
            $log.Writer.Dispose()
        }
    }

    return [pscustomobject]@{
        Outcome      = $outcome
        ExitCode     = $exitCode
        Running      = $running
        LogPath      = $log.Path
        Disconnected = $disconnected
    }
}

if (-not $LibraryOnly) {
    $projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
    $runnerPath = Join-Path $PSScriptRoot "run_idf.ps1"
    $utf8 = [Text.UTF8Encoding]::new($false)
    [Console]::OutputEncoding = $utf8
    [Console]::InputEncoding = $utf8

    while ($true) {
        $selectedPort = Show-PortPicker
        if (-not $selectedPort) {
            Write-Host "Monitor task canceled."
            break
        }
        $session = Invoke-MonitorSession -Port $selectedPort -ProjectRoot $projectRoot -RunnerPath $runnerPath -ShowOutput
        if ($session.Outcome -eq "Exited") {
            Write-Host "Monitor finished. Log saved: $($session.LogPath)"
            break
        } elseif ($session.Outcome -eq "Disconnected") {
            Write-Host "The selected USB serial port was disconnected. Select a port or press Q to exit."
        } elseif ($session.Outcome -eq "Failed") {
            Write-Host "Monitor did not start or exited with an error. Available ports:"
            foreach ($item in @(Get-PortInventory)) {
                Write-Host ("  {0} - {1}" -f $item.Port, $item.Name)
            }
            Write-Host "Select another port, enter one manually, refresh, or press Q to exit."
        }
    }
}
