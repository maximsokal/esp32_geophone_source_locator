[CmdletBinding()]
param(
    [string]$Port,
    [ValidateRange(1200, 3000000)][int]$Baud = 115200,
    [string]$LogDirectory,
    [switch]$LibraryOnly
)

# nRF54L15 serial monitor that saves the console to one UTF-8 file in logs/nrf/.
# Same idea as monitor_esp32_logs.ps1: pick a COM port, keep waiting through
# unplug/replug and node resets, never build or flash. ASCII-only file.

$ErrorActionPreference = "Stop"
# Dot-sourcing runs the ESP script's param block in this scope and overwrites
# $LibraryOnly, so remember our own switch first.
$runMonitor = -not $LibraryOnly
. (Join-Path $PSScriptRoot "monitor_esp32_logs.ps1") -LibraryOnly

function Open-UniqueNrfLog {
    param(
        [Parameter(Mandatory = $true)][string]$LogDirectory,
        [Parameter(Mandatory = $true)][string]$Port,
        [datetime]$StartedAt = (Get-Date)
    )

    $null = New-Item -ItemType Directory -Path $LogDirectory -Force
    $stamp = $StartedAt.ToString("yyyy-MM-dd_HH-mm-ss", [Globalization.CultureInfo]::InvariantCulture)
    for ($suffix = 0; $suffix -lt 1000; $suffix++) {
        $extra = if ($suffix -eq 0) { "" } else { "_{0:D2}" -f $suffix }
        $path = Join-Path $LogDirectory ("nrf_{0}_{1}{2}.log" -f $Port, $stamp, $extra)
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
    throw "Could not allocate a unique nRF monitor log name in $LogDirectory."
}

function Write-NrfLogLine {
    param(
        [Parameter(Mandatory = $true)][IO.StreamWriter]$Writer,
        [AllowEmptyString()][string]$Line,
        [switch]$ShowOutput
    )
    # Zephyr's console ends lines with CR LF; keep the file free of stray CRs.
    $clean = $Line.TrimEnd([char]13)
    $Writer.WriteLine($clean)
    $Writer.Flush()
    if ($ShowOutput) { [Console]::Out.WriteLine($clean) }
}

function Write-NrfNote {
    param(
        [Parameter(Mandatory = $true)][IO.StreamWriter]$Writer,
        [Parameter(Mandatory = $true)][string]$Message,
        [switch]$ShowOutput
    )
    $Writer.WriteLine("# $Message")
    $Writer.Flush()
    if ($ShowOutput) { Write-Host $Message }
}

function Show-NrfPortPicker {
    while ($true) {
        $inventory = @(Get-PortInventory)
        Write-Host ""
        Write-Host "NRF54L15 SERIAL MONITOR"
        if ($inventory.Count -eq 0) {
            Write-Host "No COM ports detected."
        } else {
            Write-Host "Detected serial ports:"
            for ($i = 0; $i -lt $inventory.Count; $i++) {
                $item = $inventory[$i]
                $metadata = @($item.Name, $item.Manufacturer, $(if ($item.VID) { "VID_$($item.VID)&PID_$($item.PID)" })) |
                    Where-Object { $_ } | Select-Object -Unique
                $marker = if ($item.IsEspressif) { " [Espressif - ESP32, not the nRF]" } else { "" }
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

function Invoke-NrfMonitorSession {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Port,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [int]$Baud = 115200,
        [string]$LogDirectory,
        [ValidateRange(0, 300)][double]$NoDataWarningSeconds = 5,
        [switch]$ShowOutput
    )

    $selectedPort = ConvertTo-ManualPort $Port
    if (-not $selectedPort) { throw "Invalid COM port: $Port" }
    if (-not $LogDirectory) { $LogDirectory = Join-Path $ProjectRoot "logs\nrf" }
    $startedAt = Get-Date
    $log = Open-UniqueNrfLog -LogDirectory $LogDirectory -Port $selectedPort -StartedAt $startedAt
    $relativeLog = "logs/nrf/" + [IO.Path]::GetFileName($log.Path)
    $serial = $null
    $lines = 0
    $connected = $false
    $failedOpens = 0

    try {
        Write-NrfNote $log.Writer ("Started {0:yyyy-MM-dd HH:mm:ss} | Port: {1} | Baud: {2}" -f $startedAt, $selectedPort, $Baud) -ShowOutput:$ShowOutput
        Write-NrfNote $log.Writer "Logging: WAITING FOR UART | File: $relativeLog" -ShowOutput:$ShowOutput
        Write-NrfNote $log.Writer "Waiting through port errors and resets. Ctrl+C stops the monitor." -ShowOutput:$ShowOutput

        $idleClock = [Diagnostics.Stopwatch]::StartNew()
        $noDataNoticeShown = $false
        while ($true) {
            if ($null -eq $serial) {
                $candidate = [IO.Ports.SerialPort]::new(
                    $selectedPort, $Baud, [IO.Ports.Parity]::None, 8, [IO.Ports.StopBits]::One)
                $candidate.ReadTimeout = 250
                $candidate.NewLine = "`n"
                $candidate.Encoding = [Text.UTF8Encoding]::new($false)
                $candidate.DtrEnable = $true
                try {
                    $candidate.Open()
                } catch {
                    $candidate.Dispose()
                    $failedOpens++
                    if ($failedOpens -eq 1 -or $failedOpens % 30 -eq 0) {
                        Write-NrfNote $log.Writer "Port $selectedPort unavailable ($($_.Exception.Message)); retrying every second. Capture remains open. Ctrl+C to stop." -ShowOutput:$ShowOutput
                    }
                    Start-Sleep -Seconds 1
                    continue
                }
                $serial = $candidate
                $failedOpens = 0
                $connected = $true
                $idleClock.Restart()
                $noDataNoticeShown = $false
                Write-NrfNote $log.Writer "nRF Monitor: CONNECTED | Port: $selectedPort | Baudrate: $Baud | File: $relativeLog" -ShowOutput:$ShowOutput
            }

            try {
                $line = $serial.ReadLine()
                $lines++
                $idleClock.Restart()
                $noDataNoticeShown = $false
                Write-NrfLogLine $log.Writer $line -ShowOutput:$ShowOutput
            } catch [TimeoutException] {
                if (-not $noDataNoticeShown -and $NoDataWarningSeconds -gt 0 -and
                    $idleClock.Elapsed.TotalSeconds -ge $NoDataWarningSeconds) {
                    $noDataNoticeShown = $true
                    Write-NrfNote $log.Writer "WAITING: no UART data for $([int]$NoDataWarningSeconds) s; monitor remains active." -ShowOutput:$ShowOutput
                }
            } catch [IO.IOException], [UnauthorizedAccessException], [InvalidOperationException] {
                Write-NrfNote $log.Writer "Port $selectedPort lost ($($_.Exception.Message)); reconnecting. Capture remains open." -ShowOutput:$ShowOutput
                try { $serial.Dispose() } catch {}
                $serial = $null
                Start-Sleep -Seconds 1
            }
        }
    } finally {
        if ($serial) { try { $serial.Dispose() } catch {} }
        if ($log.Writer) {
            try {
                $summary = if ($connected) { "Monitor stopped. $lines line(s) saved: $relativeLog" }
                           else { "Monitor stopped without ever opening $selectedPort. Capture file: $relativeLog" }
                Write-NrfNote $log.Writer $summary -ShowOutput:$ShowOutput
            } catch {}
            $log.Writer.Flush()
            $log.Writer.Dispose()
        }
    }
}

if ($runMonitor) {
    $projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
    $utf8 = [Text.UTF8Encoding]::new($false)
    [Console]::OutputEncoding = $utf8
    [Console]::InputEncoding = $utf8

    $selectedPort = if ($Port) { ConvertTo-ManualPort $Port } else { Show-NrfPortPicker }
    if (-not $selectedPort) {
        Write-Host "Monitor task canceled."
    } else {
        Invoke-NrfMonitorSession -Port $selectedPort -ProjectRoot $projectRoot -Baud $Baud `
            -LogDirectory $LogDirectory -ShowOutput
    }
}
