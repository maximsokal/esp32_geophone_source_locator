[CmdletBinding()]
param(
    [ValidateSet("All", "Web")]
    [string]$Suite = "All",

    # Keep the historical default for existing local tasks, while allowing a
    # clean alternate IDF build directory when another generator owns build/.
    [string]$ProductionBuildDir = "build"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$results = New-Object System.Collections.Generic.List[object]
$hostFailureDetails = New-Object System.Collections.Generic.List[object]

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

function Add-HostFailureDetails {
    param(
        [Parameter(Mandatory = $true)] [string]$SuiteName,
        [Parameter(Mandatory = $true)] [string]$ReportPath
    )

    if (-not (Test-Path -LiteralPath $ReportPath -PathType Leaf)) {
        return
    }

    try {
        $report = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
    } catch {
        $hostFailureDetails.Add([pscustomobject]@{
            Suite = $SuiteName
            Test = "<report>"
            Phase = "parse"
            ExitCode = 1
            Error = $_.Exception.Message
        })
        return
    }

    foreach ($testResult in @($report.results)) {
        if ($testResult.status -ne "FAIL") {
            continue
        }
        $hostFailureDetails.Add([pscustomobject]@{
            Suite = $SuiteName
            Test = [string]$testResult.name
            Phase = [string]$testResult.phase
            ExitCode = if ($null -eq $testResult.exit_code) { "-" } else { [string]$testResult.exit_code }
            Error = if ($null -eq $testResult.error) { "" } else { [string]$testResult.error }
        })
    }
}

function Invoke-HostSuites {
    $runHost = Join-Path $repo "tools\run_host_tests.py"
    $baseBuildDir = Join-Path $repo "build\host-tests"
    $baseReport = Join-Path $baseBuildDir "host-test-report.json"
    Remove-Item -LiteralPath $baseReport -Force -ErrorAction SilentlyContinue
    Invoke-SuiteStep -Name "Host C base" -WorkingDirectory $repo -FilePath "python" -ArgumentList @(
        $runHost,
        "--build-dir",
        $baseBuildDir,
        "--keep-going"
    )
    Add-HostFailureDetails -SuiteName "Host C base" -ReportPath $baseReport

    $manifestDirectory = Join-Path $repo "tests\host\manifest.d"
    if (-not (Test-Path -LiteralPath $manifestDirectory -PathType Container)) {
        return
    }

    $manifests = @(
        Get-ChildItem -LiteralPath $manifestDirectory -Filter "*.json" -File |
            Sort-Object Name
    )
    foreach ($manifest in $manifests) {
        $suiteName = [System.IO.Path]::GetFileNameWithoutExtension($manifest.Name)
        $buildDir = Join-Path $repo ("build\host-tests-{0}" -f $suiteName)
        $reportPath = Join-Path $buildDir "host-test-report.json"
        Remove-Item -LiteralPath $reportPath -Force -ErrorAction SilentlyContinue
        $stepName = "Host C {0}" -f $suiteName
        Invoke-SuiteStep -Name $stepName -WorkingDirectory $repo -FilePath "python" -ArgumentList @(
            $runHost,
            "--manifest",
            $manifest.FullName,
            "--build-dir",
            $buildDir,
            "--keep-going"
        )
        Add-HostFailureDetails -SuiteName $stepName -ReportPath $reportPath
    }
}

$runIdf = Join-Path $repo "tools\run_idf.ps1"
$powershell = "powershell.exe"

if ($Suite -eq "All") {
    Invoke-HostSuites
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
        "-Action", "build", "-ProjectPath", ".", "-BuildDir", $ProductionBuildDir, "-Jobs", "2"
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
Write-Host ("{0,-32} {1,-9} {2,8} {3,10}" -f "STEP", "STATUS", "EXIT", "SECONDS")
Write-Host ("{0,-32} {1,-9} {2,8} {3,10}" -f ("-" * 32), ("-" * 9), ("-" * 8), ("-" * 10))
foreach ($result in $results) {
    $color = switch ($result.Status) {
        "PASS" { "Green" }
        "SKIPPED" { "DarkYellow" }
        "BLOCKED" { "Yellow" }
        default { "Red" }
    }
    Write-Host ("{0,-32} {1,-9} {2,8} {3,10:N1}" -f $result.Name, $result.Status, $result.ExitCode, $result.Duration) -ForegroundColor $color
}

if ($hostFailureDetails.Count -gt 0) {
    Write-Host ""
    Write-Host "HOST_FAILURE_DETAILS:" -ForegroundColor Red
    foreach ($failure in $hostFailureDetails) {
        $detail = "- {0}: test={1} phase={2} exit={3}" -f $failure.Suite, $failure.Test, $failure.Phase, $failure.ExitCode
        if (-not [string]::IsNullOrWhiteSpace($failure.Error)) {
            $detail += " error=" + $failure.Error
        }
        Write-Host $detail -ForegroundColor Red
    }
}

Write-Host ("SUITE_RESULT: {0}" -f $suiteStatus) -ForegroundColor $(if ($suiteStatus -eq "PASS") { "Green" } elseif ($suiteStatus -eq "BLOCKED") { "Yellow" } else { "Red" })
if ($Suite -eq "All") {
    Write-Host "HIL_RESULT: SKIPPED (Tests: All is hardware-free; use ESP: Unity tests (flash COM4 + logs COM6) for board evidence.)" -ForegroundColor DarkYellow
}

if ($suiteStatus -eq "PASS") {
    exit 0
}
exit 1
