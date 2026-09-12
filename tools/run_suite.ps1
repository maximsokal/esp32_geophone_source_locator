[CmdletBinding()]
param(
    [ValidateSet("All", "Web")]
    [string]$Suite = "All"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$results = New-Object System.Collections.Generic.List[object]

function Invoke-SuiteStep {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$WorkingDirectory,
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [Parameter(Mandatory = $true)] [string[]]$ArgumentList
    )

    $started = Get-Date
    $status = "PASS"
    $exitCode = 0
    $message = ""
    $resolvedCommand = Get-Command $FilePath -ErrorAction SilentlyContinue

    Write-Host ""
    Write-Host ("=== {0} ===" -f $Name) -ForegroundColor Cyan
    Write-Host ("> {0} {1}" -f $FilePath, ($ArgumentList -join " ")) -ForegroundColor DarkGray

    if ($null -eq $resolvedCommand) {
        $status = "BLOCKED"
        $exitCode = 127
        $message = "Required command was not found: $FilePath"
        Write-Host $message -ForegroundColor Yellow
    } else {
        try {
            Push-Location -LiteralPath $WorkingDirectory
            try {
                & $FilePath @ArgumentList
                $exitCode = if ($null -eq $LASTEXITCODE) { 0 } else { [int]$LASTEXITCODE }
            } finally {
                Pop-Location
            }

            if ($exitCode -ne 0) {
                $status = "FAIL"
                $message = "Command exited with code $exitCode"
            }
        } catch {
            $status = "FAIL"
            $exitCode = 1
            $message = $_.Exception.Message
        }
    }

    if ($status -eq "PASS") {
        Write-Host ("{0}: PASS" -f $Name) -ForegroundColor Green
    } else {
        Write-Host ("{0}: {1} ({2})" -f $Name, $status, $message) -ForegroundColor $(if ($status -eq "BLOCKED") { "Yellow" } else { "Red" })
    }

    $results.Add([pscustomobject]@{
        Name = $Name
        Status = $status
        ExitCode = $exitCode
        Duration = ((Get-Date) - $started).TotalSeconds
    })
}

$runHost = Join-Path $repo "tools\run_host_tests.py"
$runIdf = Join-Path $repo "tools\run_idf.ps1"
$powershell = "powershell.exe"

if ($Suite -eq "All") {
    Invoke-SuiteStep -Name "Host C tests" -WorkingDirectory $repo -FilePath "python" -ArgumentList @($runHost, "--keep-going")
    Invoke-SuiteStep -Name "Python tests" -WorkingDirectory $repo -FilePath "python" -ArgumentList @("-m", "pytest", "-q")
}

if ($Suite -eq "Web" -or $Suite -eq "All") {
    Invoke-SuiteStep -Name "Web Console tests" -WorkingDirectory (Join-Path $repo "webui") -FilePath "npm" -ArgumentList @("run", "test:console")
    Invoke-SuiteStep -Name "Web XR tests" -WorkingDirectory (Join-Path $repo "webui") -FilePath "npm" -ArgumentList @("run", "test:xr")
    Invoke-SuiteStep -Name "Web typecheck" -WorkingDirectory (Join-Path $repo "webui") -FilePath "npm" -ArgumentList @("run", "typecheck")
}

if ($Suite -eq "All") {
    Invoke-SuiteStep -Name "ESP Unity build" -WorkingDirectory $repo -FilePath $powershell -ArgumentList @(
        "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $runIdf,
        "-Action", "build", "-ProjectPath", "tests/target/test_app", "-BuildDir", "build-s3",
        "-Target", "esp32s3", "-Jobs", "2"
    )
    Invoke-SuiteStep -Name "ESP production build" -WorkingDirectory $repo -FilePath $powershell -ArgumentList @(
        "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $runIdf,
        "-Action", "build", "-ProjectPath", ".", "-BuildDir", "build", "-Jobs", "2"
    )

    $results.Add([pscustomobject]@{
        Name = "ESP Unity HIL (COM4+COM6)"
        Status = "SKIPPED"
        ExitCode = 0
        Duration = 0
    })
}

$failed = @($results | Where-Object { $_.Status -eq "FAIL" })
$blocked = @($results | Where-Object { $_.Status -eq "BLOCKED" })
$suiteStatus = if ($failed.Count -gt 0) { "FAIL" } elseif ($blocked.Count -gt 0) { "BLOCKED" } else { "PASS" }

Write-Host ""
Write-Host ("===== {0} suite report =====" -f $Suite) -ForegroundColor Cyan
Write-Host ("{0,-24} {1,-9} {2,8} {3,10}" -f "STEP", "STATUS", "EXIT", "SECONDS")
Write-Host ("{0,-24} {1,-9} {2,8} {3,10}" -f ("-" * 24), ("-" * 9), ("-" * 8), ("-" * 10))
foreach ($result in $results) {
    $color = switch ($result.Status) {
        "PASS" { "Green" }
        "SKIPPED" { "DarkYellow" }
        "BLOCKED" { "Yellow" }
        default { "Red" }
    }
    Write-Host ("{0,-24} {1,-9} {2,8} {3,10:N1}" -f $result.Name, $result.Status, $result.ExitCode, $result.Duration) -ForegroundColor $color
}
Write-Host ("SUITE_RESULT: {0}" -f $suiteStatus) -ForegroundColor $(if ($suiteStatus -eq "PASS") { "Green" } elseif ($suiteStatus -eq "BLOCKED") { "Yellow" } else { "Red" })
if ($Suite -eq "All") {
    Write-Host "HIL_RESULT: SKIPPED (Tests: All is hardware-free; use ESP: Unity tests (flash COM4 + logs COM6) for board evidence.)" -ForegroundColor DarkYellow
}

if ($suiteStatus -eq "PASS") {
    exit 0
}
exit 1
