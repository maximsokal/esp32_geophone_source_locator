[CmdletBinding()]
param(
    [ValidateSet("All", "Web", "Host")]
    [string]$Suite = "All",

    # Keep the historical default for existing local tasks, while allowing a
    # clean alternate IDF build directory when another generator owns build/.
    [string]$ProductionBuildDir = "build"
)

$ErrorActionPreference = "Stop"
# Child Python/CMake/ninja processes can write non-ASCII output (paths,
# diagnostics) that a cp1251 PowerShell console cannot decode cleanly, which
# has previously surfaced as a spurious step failure with no real test
# regression behind it (see tools/run_idf.ps1's identical fix for idf_monitor
# UART logs). Force UTF-8 for every child process this suite launches.
$env:PYTHONUTF8 = "1"
$env:PYTHONIOENCODING = "utf-8"
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
        # Pass/fail is decided below from the real $LASTEXITCODE, not from
        # PowerShell's own error stream. A child process (python, cmake,
        # ninja...) writing a diagnostic line to stderr must never itself
        # abort this step under "Stop" -- that previously surfaced as a
        # spurious FAIL (e.g. a benign CMake warning, or a pytest atexit
        # cleanup message) even though the underlying command exited 0 and
        # did all of its real work correctly.
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
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
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
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

function Compress-DiagnosticText {
    param(
        [AllowNull()] [string]$Text,
        [int]$Limit = 1000
    )

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return ""
    }

    $compressed = (($Text -replace "`r?`n", " | ") -replace "\s+", " ").Trim()
    if ($compressed.Length -le $Limit) {
        return $compressed
    }
    return $compressed.Substring(0, $Limit) + "..."
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
            Compiler = "<unknown>"
            Test = "<report>"
            Phase = "parse"
            ExitCode = 1
            Diagnostic = $_.Exception.Message
        })
        return
    }

    $compilerDescription = if ([string]::IsNullOrWhiteSpace([string]$report.compiler_description)) {
        "<unknown>"
    } else {
        [string]$report.compiler_description
    }

    foreach ($testResult in @($report.results)) {
        if ($testResult.status -ne "FAIL") {
            continue
        }

        $diagnostic = ""
        if (-not [string]::IsNullOrWhiteSpace([string]$testResult.stderr)) {
            $diagnostic = Compress-DiagnosticText -Text ([string]$testResult.stderr)
        } elseif (-not [string]::IsNullOrWhiteSpace([string]$testResult.stdout)) {
            $diagnostic = Compress-DiagnosticText -Text ([string]$testResult.stdout)
        } elseif (-not [string]::IsNullOrWhiteSpace([string]$testResult.error)) {
            $diagnostic = Compress-DiagnosticText -Text ([string]$testResult.error)
        }

        $hostFailureDetails.Add([pscustomobject]@{
            Suite = $SuiteName
            Compiler = $compilerDescription
            Test = [string]$testResult.name
            Phase = [string]$testResult.phase
            ExitCode = if ($null -eq $testResult.exit_code) { "-" } else { [string]$testResult.exit_code }
            Diagnostic = $diagnostic
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
$nrfNcsWrapper = Join-Path $repo "nRF54L15\ncs.ps1"

function Add-NrfSoftwareTestBoundary {
    $nrfTestDirectory = Join-Path $repo "nRF54L15\test"
    $nrfSoftwareSuites = @()

    if (Test-Path -LiteralPath $nrfTestDirectory -PathType Container) {
        $nrfSoftwareSuites = @(
            Get-ChildItem -LiteralPath $nrfTestDirectory -Directory -Filter "test_sw_*" |
                Sort-Object Name
        )
    }

    $message = if ($nrfSoftwareSuites.Count -eq 0) {
        "no qualified NCS software/native test/test_sw_* suite exists; physical/HIL suites are excluded from Run All"
    } else {
        "test_sw_* suites exist, but the NCS-native embedded test runner has not been reviewed; PlatformIO compatibility tests are excluded from canonical production verification"
    }

    Write-Host "nRF54L15 tests: SKIPPED ($message)." -ForegroundColor Yellow
    $results.Add([pscustomobject]@{
        Name = "nRF54L15 tests"
        Status = "SKIPPED"
        ExitCode = 0
        Duration = 0
    })
}

if ($Suite -eq "Host" -or $Suite -eq "All") {
    Invoke-HostSuites
}

if ($Suite -eq "All") {
    Invoke-SuiteStep -Name "Python tests" -WorkingDirectory $repo -FilePath "python" -ArgumentList @("-m", "pytest", "-q")
    Invoke-SuiteStep -Name "nRF54L15 NCS production build" -WorkingDirectory $repo -FilePath $powershell -ArgumentList @(
        "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $nrfNcsWrapper,
        "-Action", "build"
    )
    Add-NrfSoftwareTestBoundary
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
        Write-Host ("- {0}: test={1} phase={2} exit={3}" -f $failure.Suite, $failure.Test, $failure.Phase, $failure.ExitCode) -ForegroundColor Red
        Write-Host ("  compiler={0}" -f $failure.Compiler) -ForegroundColor DarkRed
        if (-not [string]::IsNullOrWhiteSpace($failure.Diagnostic)) {
            Write-Host ("  diagnostic={0}" -f $failure.Diagnostic) -ForegroundColor DarkRed
        }
    }
}

Write-Host ("SUITE_RESULT: {0}" -f $suiteStatus) -ForegroundColor $(if ($suiteStatus -eq "PASS") { "Green" } elseif ($suiteStatus -eq "BLOCKED") { "Yellow" } else { "Red" })
if ($Suite -eq "All") {
    Write-Host "HIL_RESULT: SKIPPED (Tests: All is hardware-free; use explicit ESP/nRF HIL tasks for physical-board evidence.)" -ForegroundColor DarkYellow
}

if ($suiteStatus -eq "PASS") {
    exit 0
}
exit 1
