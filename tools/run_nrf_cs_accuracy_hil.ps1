[CmdletBinding(DefaultParameterSetName = "Replay")]
param(
    [Parameter(ParameterSetName = "Replay", Mandatory = $true)]
    [string]$LogPath,

    [Parameter(ParameterSetName = "Live", Mandatory = $true)]
    [string]$Port,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 100000000)]
    [int]$ExpectedDistanceMm,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 100000000)]
    [int]$MaxErrorMm,

    [ValidateRange(1, 100)]
    [int]$Repeats = 20,

    [ValidateRange(0.0, 1.0)]
    [double]$MinSuccessRate = 0.90,

    [ValidateRange(0, 1000)]
    [int]$MinConfidence = 500,

    [ValidateRange(-1, 100000000)]
    [int]$MaxStdDevMm = -1,

    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 180,

    [string]$OutputDir = "artifacts\nrf-cs-accuracy-hil",

    [switch]$FixtureConfirmed,
    [switch]$Flash,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$project = Join-Path $repo "tests\target\nrf_cs_accuracy_hil"
$resolvedOutputDir = if ([IO.Path]::IsPathRooted($OutputDir)) {
    [IO.Path]::GetFullPath($OutputDir)
} else {
    [IO.Path]::GetFullPath((Join-Path $repo $OutputDir))
}
$buildDir = Join-Path $resolvedOutputDir "build"

New-Item -ItemType Directory -Force -Path $resolvedOutputDir | Out-Null

function Invoke-PowerShellChecked([string[]]$Arguments) {
    Write-Host ("> powershell.exe {0}" -f ($Arguments -join " "))
    & powershell.exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "PowerShell child command failed with exit code $LASTEXITCODE"
    }
}

function Resolve-InputLog([string]$Path) {
    $candidate = if ([IO.Path]::IsPathRooted($Path)) {
        $Path
    } else {
        Join-Path $repo $Path
    }
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "Channel Sounding HIL log not found: $candidate"
    }
    return [IO.Path]::GetFullPath($candidate)
}

function Capture-LiveLog([string]$SerialPortName, [string]$CapturePath) {
    $serial = [System.IO.Ports.SerialPort]::new(
        $SerialPortName,
        115200,
        [System.IO.Ports.Parity]::None,
        8,
        [System.IO.Ports.StopBits]::One
    )
    $serial.ReadTimeout = 250
    $serial.DtrEnable = $true
    $lines = [System.Collections.Generic.List[string]]::new()
    $watch = [Diagnostics.Stopwatch]::StartNew()

    Write-Host "Opening ESP32 HIL capture port $SerialPortName @ 115200..." -ForegroundColor Cyan
    try {
        $serial.Open()
        while ($watch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
            try {
                $line = $serial.ReadLine()
                $lines.Add($line)
                Write-Host $line
                if ($line -match '^NRF_CS_ACCURACY_HIL_END\s*$') {
                    break
                }
            }
            catch [System.TimeoutException] {
                # Keep polling so the timeout and Ctrl+C behavior remain bounded.
            }
        }
    }
    finally {
        if ($serial.IsOpen) {
            $serial.Close()
        }
        $serial.Dispose()
        [IO.File]::WriteAllLines($CapturePath, $lines)
    }

    return $CapturePath
}

function Read-HilSamples([string]$Path) {
    $samples = [System.Collections.Generic.List[object]]::new()
    $parseErrors = [System.Collections.Generic.List[string]]::new()
    $topologyReady = $false

    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^NRF_CS_ACCURACY_HIL_TOPOLOGY\s+.*\sassigned=2\s*$') {
            $topologyReady = $true
        }
        if ($line -notmatch '^NRF_CS_ACCURACY_HIL_SAMPLE\s+(\{.*\})\s*$') {
            continue
        }

        try {
            $samples.Add(($matches[1] | ConvertFrom-Json))
        }
        catch {
            $parseErrors.Add($_.Exception.Message)
        }
    }

    return [pscustomobject]@{
        Samples = $samples.ToArray()
        ParseErrors = $parseErrors.ToArray()
        TopologyReady = $topologyReady
    }
}

function Get-Percentile([double[]]$Values, [double]$Fraction) {
    if ($Values.Count -eq 0) {
        return $null
    }
    $sorted = @($Values | Sort-Object)
    $index = [int][Math]::Ceiling($sorted.Count * $Fraction) - 1
    $index = [Math]::Max(0, [Math]::Min($sorted.Count - 1, $index))
    return [double]$sorted[$index]
}

function Get-StandardDeviation([double[]]$Values) {
    if ($Values.Count -lt 2) {
        return 0.0
    }
    $mean = ($Values | Measure-Object -Average).Average
    $sum = 0.0
    foreach ($value in $Values) {
        $delta = $value - $mean
        $sum += $delta * $delta
    }
    return [Math]::Sqrt($sum / ($Values.Count - 1))
}

function Evaluate-HilSamples(
    [object[]]$Samples,
    [bool]$TopologyReady,
    [int]$ParseErrorCount)
{
    $all = @($Samples | Where-Object {
        $_.pair_count -eq 1 -and $_.pair_index -eq 0
    })
    $fatalSchedulerQuality = 2 + 4 + 8 + 16 + 32
    $accepted = @($all | Where-Object {
        $_.state -eq 2 -and
        $_.distance_mm -ge 0 -and
        $_.confidence -ge $MinConfidence -and
        (($_.scheduler_quality_flags -band $fatalSchedulerQuality) -eq 0)
    })
    $qualityRejected = @($all | Where-Object {
        $_.state -eq 2 -and
        (($_.scheduler_quality_flags -band $fatalSchedulerQuality) -ne 0 -or
         $_.confidence -lt $MinConfidence)
    })

    $distances = @($accepted | ForEach-Object { [double]$_.distance_mm })
    $errors = @($accepted | ForEach-Object {
        [Math]::Abs([double]$_.distance_mm - $ExpectedDistanceMm)
    })
    $successRate = if ($all.Count -gt 0) {
        [double]$accepted.Count / [double]$all.Count
    } else {
        0.0
    }
    $mean = if ($distances.Count -gt 0) {
        [double](($distances | Measure-Object -Average).Average)
    } else {
        $null
    }
    $bias = if ($null -ne $mean) { $mean - $ExpectedDistanceMm } else { $null }
    $p95 = Get-Percentile $errors 0.95
    $maxError = if ($errors.Count -gt 0) { [double](($errors | Measure-Object -Maximum).Maximum) } else { $null }
    $stddev = Get-StandardDeviation $distances
    $stddevLimit = if ($MaxStdDevMm -lt 0) { [double]$MaxErrorMm } else { [double]$MaxStdDevMm }

    $reasons = [System.Collections.Generic.List[string]]::new()
    if (-not $TopologyReady) { $reasons.Add("missing two-node topology marker") }
    if ($ParseErrorCount -gt 0) { $reasons.Add("invalid sample JSON") }
    if ($all.Count -lt $Repeats) { $reasons.Add("sample count $($all.Count) is below requested $Repeats") }
    if ($successRate -lt $MinSuccessRate) { $reasons.Add("success rate $successRate is below $MinSuccessRate") }
    if ($accepted.Count -eq 0) { $reasons.Add("no quality-approved valid pair samples") }
    if ($null -ne $p95 -and $p95 -gt $MaxErrorMm) { $reasons.Add("p95 error $p95 mm exceeds $MaxErrorMm mm") }
    if ($null -ne $maxError -and $maxError -gt $MaxErrorMm) { $reasons.Add("maximum error $maxError mm exceeds $MaxErrorMm mm") }
    if ($null -ne $bias -and [Math]::Abs($bias) -gt $MaxErrorMm) { $reasons.Add("absolute bias $([Math]::Abs($bias)) mm exceeds $MaxErrorMm mm") }
    if ($stddev -gt $stddevLimit) { $reasons.Add("repeatability standard deviation $stddev mm exceeds $stddevLimit mm") }

    $evidenceComplete =
        $TopologyReady -and
        $all.Count -ge $Repeats -and
        $accepted.Count -gt 0
    $metricsPass =
        $evidenceComplete -and
        $successRate -ge $MinSuccessRate -and
        $null -ne $p95 -and $p95 -le $MaxErrorMm -and
        $null -ne $maxError -and $maxError -le $MaxErrorMm -and
        $null -ne $bias -and [Math]::Abs($bias) -le $MaxErrorMm -and
        $stddev -le $stddevLimit

    $status = if (-not $evidenceComplete) {
        "BLOCKED"
    } elseif (-not $metricsPass) {
        "FAIL"
    } elseif (-not $FixtureConfirmed) {
        $reasons.Add("fixture confirmation was not supplied")
        "BLOCKED"
    } else {
        "PASS"
    }

    return [pscustomobject][ordered]@{
        status = $status
        expected_distance_mm = $ExpectedDistanceMm
        max_error_mm = $MaxErrorMm
        max_stddev_mm = $stddevLimit
        requested_repeats = $Repeats
        sample_count = $all.Count
        accepted_sample_count = $accepted.Count
        quality_rejected_count = $qualityRejected.Count
        success_rate = [Math]::Round($successRate, 6)
        mean_distance_mm = if ($null -ne $mean) { [Math]::Round($mean, 3) } else { $null }
        bias_mm = if ($null -ne $bias) { [Math]::Round($bias, 3) } else { $null }
        p95_abs_error_mm = if ($null -ne $p95) { [Math]::Round($p95, 3) } else { $null }
        max_abs_error_mm = if ($null -ne $maxError) { [Math]::Round($maxError, 3) } else { $null }
        repeatability_stddev_mm = [Math]::Round($stddev, 3)
        topology_ready = $TopologyReady
        fixture_confirmed = [bool]$FixtureConfirmed
        parse_error_count = $ParseErrorCount
        failure_reasons = @($reasons)
    }
}

try {
    if ($PSCmdlet.ParameterSetName -eq "Replay" -and $Flash) {
        throw "-Flash is only valid with a live -Port capture."
    }
    if ($PSCmdlet.ParameterSetName -eq "Live" -and $Flash -and $SkipBuild) {
        Write-Host "Live flash requested with -SkipBuild; using the existing HIL build." -ForegroundColor Yellow
    }

    if ($PSCmdlet.ParameterSetName -eq "Live") {
        $runIdf = Join-Path $repo "tools\run_idf.ps1"
        if (-not $SkipBuild) {
            Invoke-PowerShellChecked @(
                "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", $runIdf,
                "-Action", "build",
                "-ProjectPath", $project,
                "-BuildDir", $buildDir,
                "-Target", "esp32s3",
                "-Jobs", "2"
            )
        }
        if ($Flash) {
            Invoke-PowerShellChecked @(
                "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", $runIdf,
                "-Action", "flash",
                "-ProjectPath", $project,
                "-BuildDir", $buildDir,
                "-Port", $Port
            )
        }
        $capturePath = Join-Path $resolvedOutputDir ("capture-{0}.log" -f (Get-Date -Format "yyyyMMdd-HHmmss"))
        $inputLog = Capture-LiveLog $Port $capturePath
    } else {
        $inputLog = Resolve-InputLog $LogPath
    }

    $parsed = Read-HilSamples $inputLog
    $summary = Evaluate-HilSamples $parsed.Samples $parsed.TopologyReady $parsed.ParseErrors.Count
    $summary | Add-Member -NotePropertyName input_log -NotePropertyValue $inputLog
    $summaryPath = Join-Path $resolvedOutputDir "summary.json"
    [IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 6))

    Write-Host ""
    Write-Host ("NRF_CS_ACCURACY_HIL_RESULT: {0}" -f $summary.status) -ForegroundColor $(
        if ($summary.status -eq "PASS") { "Green" } elseif ($summary.status -eq "FAIL") { "Red" } else { "Yellow" }
    )
    Write-Host ($summary | ConvertTo-Json -Depth 6)
    Write-Host "Summary: $summaryPath"

    if ($summary.status -eq "PASS") { exit 0 }
    if ($summary.status -eq "FAIL") { exit 1 }
    exit 2
}
catch {
    Write-Host "NRF_CS_ACCURACY_HIL_RESULT: BLOCKED" -ForegroundColor Yellow
    Write-Host ("reason={0}" -f $_.Exception.Message)
    exit 2
}
