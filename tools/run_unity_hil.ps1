[CmdletBinding()]
param(
    [string]$FlashPort = "COM4",
    [string]$LogPort = "COM6",
    [string]$ProjectPath = "test_app",
    [string]$BuildDir = "build-s3",

    [ValidateRange(5, 300)]
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$project = if ([IO.Path]::IsPathRooted($ProjectPath)) {
    (Resolve-Path -LiteralPath $ProjectPath).Path
} else {
    (Resolve-Path -LiteralPath (Join-Path $repo $ProjectPath)).Path
}
$runIdf = Join-Path $PSScriptRoot "run_idf.ps1"
$checkPorts = Join-Path $PSScriptRoot "check_serial_ports.ps1"
$powershell = (Get-Command powershell.exe -ErrorAction Stop).Source
$logDir = Join-Path $repo "build\hil"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$stdoutPath = Join-Path $logDir "unity-monitor-$stamp.log"
$stderrPath = Join-Path $logDir "unity-monitor-$stamp.err.log"
$monitor = $null
$passed = $false

if (-not (Test-Path -LiteralPath $runIdf -PathType Leaf)) {
    throw "Missing ESP-IDF task helper: $runIdf"
}
if (-not (Test-Path -LiteralPath $checkPorts -PathType Leaf)) {
    throw "Missing serial-port helper: $checkPorts"
}

New-Item -ItemType Directory -Path $logDir -Force | Out-Null
& $checkPorts -Ports @($FlashPort, $LogPort)
if (-not $?) {
    throw "Serial port check failed."
}

try {
    $monitorArgs = @(
        "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"{0}"' -f $runIdf),
        "-Action", "monitor", "-ProjectPath", $ProjectPath,
        "-BuildDir", $BuildDir, "-Port", $LogPort
    )
    Write-Host ("Starting Unity monitor on {0}; output: {1}" -f $LogPort, $stdoutPath)
    $monitor = Start-Process -FilePath $powershell -ArgumentList $monitorArgs -WorkingDirectory $project -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru -WindowStyle Hidden

    Start-Sleep -Seconds 2
    if ($monitor.HasExited) {
        throw "The COM6 monitor exited before flashing. See $stderrPath."
    }

    & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $runIdf -Action flash -ProjectPath $ProjectPath -BuildDir $BuildDir -Port $FlashPort
    if ($LASTEXITCODE -ne 0) {
        throw "Unity test-app flash failed with exit code $LASTEXITCODE."
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) {
            $content = Get-Content -LiteralPath $stdoutPath -Raw -ErrorAction SilentlyContinue
            if ($content -match "TEST_APP_RESULT:\s*PASS") {
                $passed = $true
                break
            }
            if ($content -match "TEST_APP_RESULT:\s*FAIL") {
                break
            }
        }
        if ($monitor.HasExited) {
            break
        }
        Start-Sleep -Milliseconds 250
    }

    Write-Host "----- Unity monitor stdout -----"
    if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) {
        Get-Content -LiteralPath $stdoutPath
    }
    Write-Host "----- Unity monitor stderr -----"
    if (Test-Path -LiteralPath $stderrPath -PathType Leaf) {
        Get-Content -LiteralPath $stderrPath
    }

    if (-not $passed) {
        throw "Unity test result marker was not PASS within $TimeoutSeconds seconds."
    }
    Write-Host "Unity HIL result: PASS"
} finally {
    if ($null -ne $monitor -and -not $monitor.HasExited) {
        Stop-Process -Id $monitor.Id -Force -ErrorAction SilentlyContinue
        $monitor.WaitForExit()
    }
}
