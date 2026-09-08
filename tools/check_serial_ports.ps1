[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Ports
)

$ErrorActionPreference = "Stop"

$requested = @(
    $Ports |
        ForEach-Object { $_ -split "," } |
        ForEach-Object { $_.Trim().ToUpperInvariant() } |
        Where-Object { $_ -ne "" } |
        Select-Object -Unique
)

if ($requested.Count -eq 0) {
    throw "At least one serial port is required."
}

$records = @()
try {
    $records = @(Get-CimInstance Win32_SerialPort -ErrorAction Stop)
} catch {
    $records = @()
}

try {
    $available = @(
        [System.IO.Ports.SerialPort]::GetPortNames() |
            ForEach-Object { $_.ToUpperInvariant() } |
            Select-Object -Unique
    )
} catch {
    $available = @()
}

$missing = @($requested | Where-Object { $_ -notin $available })
if ($missing.Count -gt 0) {
    $pnp = @(
        Get-PnpDevice -Class Ports -ErrorAction SilentlyContinue |
            ForEach-Object {
                if ($_.FriendlyName -match "\((COM\d+)\)") {
                    "{0} [{1}]" -f $Matches[1].ToUpperInvariant(), $_.Status
                }
            } |
            Select-Object -Unique
    )
    $shown = if ($available.Count -gt 0) { $available -join ", " } else { "none" }
    $pnpShown = if ($pnp.Count -gt 0) { $pnp -join ", " } else { "none" }
    throw "Required live serial port(s) missing: $($missing -join ', '). Live ports: $shown. PnP entries: $pnpShown."
}

foreach ($port in $requested) {
    $record = $records | Where-Object { $_.DeviceID.ToUpperInvariant() -eq $port } | Select-Object -First 1
    if ($null -ne $record) {
        Write-Host ("{0}: {1}" -f $port, $record.Name)
    } else {
        Write-Host ("{0}: present (PnP enumeration)" -f $port)
    }
}
