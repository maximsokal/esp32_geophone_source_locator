[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Hex,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$BuildAppDir,

    [ValidateNotNullOrEmpty()]
    [string]$OpenOcd,

    [ValidateNotNullOrEmpty()]
    [string]$ScriptsDir,

    [ValidateNotNullOrEmpty()]
    [string]$BoardCfg,

    [ValidateRange(4, 2147483647)]
    [int]$Bytes = 64
)

$ErrorActionPreference = "Stop"

function Read-IhexImage([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Intel HEX file not found: $Path"
    }

    $rawBytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path).Path)
    foreach ($value in $rawBytes) {
        if ($value -gt 0x7F) {
            throw "Input is not plain ASCII Intel HEX: $Path"
        }
    }

    $text = [System.Text.Encoding]::ASCII.GetString($rawBytes)
    $splitLines = [System.Text.RegularExpressions.Regex]::Split($text, "`r`n|`n|`r")
    $lineCount = $splitLines.Length
    if ($text.EndsWith("`n") -or $text.EndsWith("`r")) {
        $lineCount--
    }
    if ($lineCount -le 0) {
        throw "Intel HEX is empty or missing its EOF record: $Path"
    }

    $dataBytes = @{}
    $records = New-Object 'System.Collections.Generic.List[object]'
    [long]$activeBase = 0
    $sawEof = $false
    for ($lineIndex = 0; $lineIndex -lt $lineCount; $lineIndex++) {
        $lineNumber = $lineIndex + 1
        $line = $splitLines[$lineIndex]
        $match = [System.Text.RegularExpressions.Regex]::Match(
            $line,
            '^:([0-9A-Fa-f]{2})([0-9A-Fa-f]{4})([0-9A-Fa-f]{2})([0-9A-Fa-f]*)([0-9A-Fa-f]{2})$'
        )
        if (-not $match.Success) {
            throw "Malformed Intel HEX record at line $lineNumber."
        }

        $count = [Convert]::ToInt32($match.Groups[1].Value, 16)
        $address = [Convert]::ToInt32($match.Groups[2].Value, 16)
        $recordType = [Convert]::ToInt32($match.Groups[3].Value, 16)
        $dataHex = $match.Groups[4].Value
        $checksum = [Convert]::ToInt32($match.Groups[5].Value, 16)
        if ($line.Length -ne (11 + (2 * $count))) {
            throw "Intel HEX byte count does not match record length at line $lineNumber."
        }

        $sum = $count + (($address -shr 8) -band 0xFF) + ($address -band 0xFF) + $recordType + $checksum
        for ($byteIndex = 0; $byteIndex -lt $count; $byteIndex++) {
            $sum += [Convert]::ToInt32($dataHex.Substring(2 * $byteIndex, 2), 16)
        }
        if (($sum -band 0xFF) -ne 0) {
            throw "Intel HEX checksum is invalid at line $lineNumber."
        }
        if ($sawEof) {
            throw "Intel HEX contains a record after EOF at line $lineNumber."
        }

        [long]$recordBase = $activeBase
        [long]$recordStart = $activeBase + $address
        [long]$recordEnd = $recordStart + $count
        switch ($recordType) {
            0 {
                if (($address + $count) -gt 65536) {
                    throw "Data record crosses a 64 KiB offset boundary at line $lineNumber."
                }
                if ($recordEnd -gt 4294967296) {
                    throw "Data record exceeds the Intel HEX address range at line $lineNumber."
                }
                for ($byteIndex = 0; $byteIndex -lt $count; $byteIndex++) {
                    $dataBytes[[long]($recordStart + $byteIndex)] = [byte][Convert]::ToInt32(
                        $dataHex.Substring(2 * $byteIndex, 2), 16
                    )
                }
            }
            1 {
                if ($count -ne 0 -or $address -ne 0) {
                    throw "Malformed EOF record at line $lineNumber."
                }
                if ($lineIndex -ne ($lineCount - 1)) {
                    throw "Intel HEX EOF record must be the final record (line $lineNumber)."
                }
                $sawEof = $true
            }
            2 {
                if ($count -ne 2 -or $address -ne 0) {
                    throw "Malformed extended segment address record at line $lineNumber."
                }
                $activeBase = [long]([Convert]::ToInt32($dataHex, 16) * 16)
            }
            3 {
                if ($count -ne 4 -or $address -ne 0) {
                    throw "Malformed start segment address record at line $lineNumber."
                }
            }
            4 {
                if ($count -ne 2 -or $address -ne 0) {
                    throw "Malformed extended linear address record at line $lineNumber."
                }
                $activeBase = [long]([Convert]::ToInt32($dataHex, 16) * 65536)
            }
            5 {
                if ($count -ne 4 -or $address -ne 0) {
                    throw "Malformed start linear address record at line $lineNumber."
                }
            }
            default {
                throw "Unsupported Intel HEX record type 0x$('{0:X2}' -f $recordType) at line $lineNumber."
            }
        }

        [void]$records.Add([pscustomobject]@{
            Index = $lineIndex
            Type = $recordType
            Address = $address
            Base = $recordBase
            Start = $recordStart
            End = $recordEnd
        })
    }

    if (-not $sawEof) {
        throw "Intel HEX is missing its EOF record: $Path"
    }
    if ($dataBytes.Count -eq 0) {
        throw "Intel HEX contains no non-empty data records: $Path"
    }
    return [pscustomobject]@{ Bytes = $dataBytes; Records = $records }
}

function Get-ContiguousRuns([hashtable]$DataBytes) {
    [long[]]$addresses = @($DataBytes.Keys | ForEach-Object { [long]$_ })
    [Array]::Sort($addresses)
    $runs = New-Object 'System.Collections.Generic.List[object]'
    [long]$start = $addresses[0]
    [long]$previous = $addresses[0]
    for ($index = 1; $index -lt $addresses.Length; $index++) {
        [long]$address = $addresses[$index]
        if ($address -ne ($previous + 1)) {
            [void]$runs.Add([pscustomobject]@{ Start = $start; End = ($previous + 1) })
            $start = $address
        }
        $previous = $address
    }
    [void]$runs.Add([pscustomobject]@{ Start = $start; End = ($previous + 1) })
    return $runs.ToArray()
}

function Add-ReadWindow(
    [hashtable]$DataBytes,
    [hashtable]$CompareBytes,
    [hashtable]$RequestedWords,
    [hashtable]$SpanKeys,
    [System.Collections.Generic.List[object]]$ReadSpans,
    [long]$Start,
    [long]$End
) {
    if ($End -le $Start) {
        return
    }

    [long]$availableEnd = $Start
    while ($availableEnd -lt $End -and $DataBytes.ContainsKey($availableEnd)) {
        $CompareBytes[$availableEnd] = $true
        $availableEnd++
    }
    if ($availableEnd -le $Start) {
        return
    }

    [long]$wordStart = [long]([Math]::Floor($Start / 4.0) * 4)
    [long]$wordEnd = [long]([Math]::Ceiling($availableEnd / 4.0) * 4)
    if ($wordStart -gt 4294967292 -or $wordEnd -gt 4294967296) {
        throw "Requested read window exceeds the Intel HEX address range at 0x$('{0:x}' -f $Start)."
    }
    [int]$wordCount = [int](($wordEnd - $wordStart) / 4)
    $spanKey = "$wordStart/$wordCount"
    if (-not $SpanKeys.ContainsKey($spanKey)) {
        $SpanKeys[$spanKey] = $true
        [void]$ReadSpans.Add([pscustomobject]@{ Start = $wordStart; Count = $wordCount })
    }
    for ([long]$wordAddress = $wordStart; $wordAddress -lt $wordEnd; $wordAddress += 4) {
        $RequestedWords[$wordAddress] = $true
    }
}

function Get-ConfiguredRunnerPaths([string]$RunnersYaml) {
    if (-not (Test-Path -LiteralPath $RunnersYaml -PathType Leaf)) {
        throw "Zephyr runners.yaml not found: $RunnersYaml"
    }

    $openOcdFromYaml = $null
    $boardDirFromYaml = $null
    $scriptsFromYaml = $null
    $insideConfig = $false
    $searchIndent = -1
    foreach ($line in [System.IO.File]::ReadAllLines($RunnersYaml)) {
        if ($line -match '^\s*(?:#.*)?$') { continue }
        $indent = ([System.Text.RegularExpressions.Regex]::Match($line, '^\s*')).Length
        if ($indent -eq 0) {
            $insideConfig = $line -match '^config:\s*(?:#.*)?$'
            $searchIndent = -1
            continue
        }
        if (-not $insideConfig) { continue }
        if ($searchIndent -ge 0 -and $indent -le $searchIndent) { $searchIndent = -1 }
        if ($searchIndent -ge 0 -and $null -eq $scriptsFromYaml) {
            $searchMatch = [System.Text.RegularExpressions.Regex]::Match($line, '^\s+-\s+(.+?)\s*$')
            if ($searchMatch.Success) {
                $scriptsFromYaml = $searchMatch.Groups[1].Value.Trim()
                continue
            }
        }
        $match = [System.Text.RegularExpressions.Regex]::Match($line, '^\s*board_dir:\s*(.*?)\s*(?:#.*)?$')
        if ($match.Success) { $boardDirFromYaml = $match.Groups[1].Value.Trim(); continue }
        $match = [System.Text.RegularExpressions.Regex]::Match($line, '^\s*openocd:\s*(.*?)\s*(?:#.*)?$')
        if ($match.Success) { $openOcdFromYaml = $match.Groups[1].Value.Trim(); continue }
        $match = [System.Text.RegularExpressions.Regex]::Match($line, '^\s*openocd_search:\s*(?:#.*)?$')
        if ($match.Success) { $searchIndent = $indent }
    }

    if ([string]::IsNullOrWhiteSpace($openOcdFromYaml) -or
        [string]::IsNullOrWhiteSpace($boardDirFromYaml) -or
        [string]::IsNullOrWhiteSpace($scriptsFromYaml)) {
        throw "runners.yaml is missing config openocd, board_dir, or the first openocd_search entry."
    }
    return [pscustomobject]@{ OpenOcd = $openOcdFromYaml; BoardDir = $boardDirFromYaml; ScriptsDir = $scriptsFromYaml }
}

function Resolve-RunnerPath([string]$Path, [string]$BaseDir) {
    $normalized = $Path.Trim()
    if (($normalized.StartsWith('"') -and $normalized.EndsWith('"')) -or
        ($normalized.StartsWith("'") -and $normalized.EndsWith("'"))) {
        $normalized = $normalized.Substring(1, $normalized.Length - 2)
    }
    $normalized = $normalized.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
    if (-not [System.IO.Path]::IsPathRooted($normalized)) { $normalized = Join-Path $BaseDir $normalized }
    return [System.IO.Path]::GetFullPath($normalized)
}

function ConvertTo-QuotedWindowsArgument([string]$Argument) {
    if ($Argument.Length -eq 0) { return '""' }
    $builder = New-Object System.Text.StringBuilder
    $backslash = [char]92
    $quote = [char]34
    $backslashes = 0
    [void]$builder.Append($quote)
    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq $backslash) { $backslashes++; continue }
        if ($character -eq $quote) {
            for ($index = 0; $index -lt ((2 * $backslashes) + 1); $index++) { [void]$builder.Append($backslash) }
            [void]$builder.Append($quote)
            $backslashes = 0
            continue
        }
        for ($index = 0; $index -lt $backslashes; $index++) { [void]$builder.Append($backslash) }
        $backslashes = 0
        [void]$builder.Append($character)
    }
    for ($index = 0; $index -lt (2 * $backslashes); $index++) { [void]$builder.Append($backslash) }
    [void]$builder.Append($quote)
    return $builder.ToString()
}

function Stop-WithOpenOcdFailure([string]$Reason, [string[]]$OutputLines) {
    [Console]::Error.WriteLine("rram tail check: OpenOCD failure: $Reason")
    if ($script:OpenOcdCommandLine) { [Console]::Error.WriteLine("rram tail check: command: $script:OpenOcdCommandLine") }
    foreach ($line in @($OutputLines | Select-Object -Last 30)) { [Console]::Error.WriteLine($line) }
    exit 1
}

try {
    if (($Bytes % 4) -ne 0) { throw "-Bytes must be a multiple of 4." }
    if (-not (Test-Path -LiteralPath $BuildAppDir -PathType Container)) {
        throw "Build app directory not found: $BuildAppDir"
    }

    $hexFullPath = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Hex).Path)
    $expectedImage = Read-IhexImage $hexFullPath
    $sourceHexPath = $hexFullPath
    if ($hexFullPath -match '(?i)\.padded\.hex$') {
        $candidate = [System.Text.RegularExpressions.Regex]::Replace(
            $hexFullPath,
            '\.padded\.hex$',
            '.hex',
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
        )
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $sourceHexPath = $candidate }
    }
    if ([string]::Equals($sourceHexPath, $hexFullPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        $sourceImage = $expectedImage
    } else {
        $sourceImage = Read-IhexImage $sourceHexPath
    }
    $runs = @(Get-ContiguousRuns $sourceImage.Bytes)
    if ($runs.Count -eq 0) { throw "Intel HEX contains no non-empty data runs: $sourceHexPath" }

    $compareBytes = @{}
    $requestedWords = @{}
    $spanKeys = @{}
    $readSpans = New-Object 'System.Collections.Generic.List[object]'
    foreach ($run in $runs) {
        [long]$lastLineStart = [long]([Math]::Floor(($run.End - 1) / 16.0) * 16)
        [long]$lastLineEnd = [long]([Math]::Ceiling($run.End / 16.0) * 16)
        [long]$lastLineStart = [Math]::Max($run.Start, $lastLineStart)
        Add-ReadWindow $expectedImage.Bytes $compareBytes $requestedWords $spanKeys $readSpans $lastLineStart $lastLineEnd

        [long]$tailStart = [Math]::Max($run.Start, ($run.End - [long]$Bytes))
        Add-ReadWindow $expectedImage.Bytes $compareBytes $requestedWords $spanKeys $readSpans $tailStart $run.End

        [long]$firstEnd = [Math]::Min($run.End, ($run.Start + 16))
        Add-ReadWindow $expectedImage.Bytes $compareBytes $requestedWords $spanKeys $readSpans $run.Start $firstEnd
    }
    if ($readSpans.Count -eq 0 -or $compareBytes.Count -eq 0) {
        throw "No complete 32-bit words are available to verify in the Intel HEX runs."
    }

    $runnersYaml = Join-Path $BuildAppDir "runners.yaml"
    $runnerPaths = Get-ConfiguredRunnerPaths $runnersYaml
    $buildAppFullPath = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $BuildAppDir).Path)
    $resolvedOpenOcd = Resolve-RunnerPath $(if ($OpenOcd) { $OpenOcd } else { $runnerPaths.OpenOcd }) $buildAppFullPath
    $resolvedScriptsDir = Resolve-RunnerPath $(if ($ScriptsDir) { $ScriptsDir } else { $runnerPaths.ScriptsDir }) $buildAppFullPath
    if ($BoardCfg) {
        $resolvedBoardCfg = Resolve-RunnerPath $BoardCfg $buildAppFullPath
    } else {
        $resolvedBoardCfg = Resolve-RunnerPath (Join-Path $runnerPaths.BoardDir "support/openocd.cfg") $buildAppFullPath
    }
    if (-not (Test-Path -LiteralPath $resolvedOpenOcd -PathType Leaf)) {
        throw "OpenOCD executable not found: $resolvedOpenOcd"
    }

    # OpenOCD evaluates -s/-f as Tcl, so a Windows backslash path would have its
    # backslashes read as escape sequences. Pass forward-slash paths, exactly like
    # runners.yaml does.
    $openOcdScriptsArg = $resolvedScriptsDir.Replace([string][char]92, "/")
    $openOcdBoardCfgArg = $resolvedBoardCfg.Replace([string][char]92, "/")
    $openOcdArguments = @("-s", $openOcdScriptsArg, "-f", $openOcdBoardCfgArg, "-c", "init", "-c", "targets nrf54l.cpu")
    foreach ($span in $readSpans) {
        $mdwCommand = "mdw 0x$('{0:x}' -f $span.Start) $($span.Count)"
        $openOcdArguments += @("-c", $mdwCommand)
    }
    # Resume the application after inspection so the target is not left halted.
    $openOcdArguments += @("-c", "reset run", "-c", "shutdown")
    $quotedArguments = @($openOcdArguments | ForEach-Object { ConvertTo-QuotedWindowsArgument ([string]$_) })
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    if ($resolvedOpenOcd.EndsWith(".cmd", [System.StringComparison]::OrdinalIgnoreCase) -or
        $resolvedOpenOcd.EndsWith(".bat", [System.StringComparison]::OrdinalIgnoreCase)) {
        $process.StartInfo.FileName = $env:ComSpec
        $process.StartInfo.Arguments = '/d /s /c ""' + $resolvedOpenOcd + '" ' + ($quotedArguments -join ' ') + '"'
    } else {
        $process.StartInfo.FileName = $resolvedOpenOcd
        $process.StartInfo.Arguments = $quotedArguments -join ' '
    }

    $script:OpenOcdCommandLine = $process.StartInfo.FileName + " " + $process.StartInfo.Arguments
    if (-not $process.Start()) { throw "OpenOCD process did not start: $resolvedOpenOcd" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $completed = $process.WaitForExit(60000)
    if (-not $completed) {
        try { $process.Kill() } catch { }
        [void]$process.WaitForExit()
    } else {
        [void]$process.WaitForExit()
    }
    $stdoutText = $stdoutTask.Result
    $stderrText = $stderrTask.Result
    $outputLines = New-Object 'System.Collections.Generic.List[string]'
    foreach ($line in [System.Text.RegularExpressions.Regex]::Split($stdoutText, "`r`n|`n|`r")) {
        if ($line.Length -gt 0) { [void]$outputLines.Add($line) }
    }
    foreach ($line in [System.Text.RegularExpressions.Regex]::Split($stderrText, "`r`n|`n|`r")) {
        if ($line.Length -gt 0) { [void]$outputLines.Add($line) }
    }
    $openOcdExitCode = $process.ExitCode
    $process.Dispose()
    if (-not $completed) { Stop-WithOpenOcdFailure "timed out after 60 seconds" $outputLines.ToArray() }

    $actualBytes = @{}
    $actualWords = @{}
    $sawMdwLine = $false
    foreach ($line in $outputLines) {
        $mdwMatch = [System.Text.RegularExpressions.Regex]::Match(
            $line,
            '^\s*0x([0-9A-Fa-f]{1,8}):\s+([0-9A-Fa-f]{8}(?:\s+[0-9A-Fa-f]{8})*)\s*$'
        )
        if (-not $mdwMatch.Success) {
            if ($line -match '(?i)\bmdw\b' -or $line -match '^\s*0x[0-9A-Fa-f]{1,8}:') {
                Stop-WithOpenOcdFailure "mdw output was unparsable" $outputLines.ToArray()
            }
            continue
        }
        $sawMdwLine = $true
        [long]$wordAddress = [Convert]::ToInt64($mdwMatch.Groups[1].Value, 16)
        $words = [System.Text.RegularExpressions.Regex]::Matches($mdwMatch.Groups[2].Value, '[0-9A-Fa-f]{8}')
        foreach ($word in $words) {
            if (-not $requestedWords.ContainsKey($wordAddress)) {
                Stop-WithOpenOcdFailure "mdw returned an unrequested word at 0x$('{0:x}' -f $wordAddress)" $outputLines.ToArray()
            }
            $wordValue = [Convert]::ToUInt32($word.Value, 16)
            for ($byteIndex = 0; $byteIndex -lt 4; $byteIndex++) {
                [long]$byteAddress = $wordAddress + $byteIndex
                [byte]$byteValue = [byte](($wordValue -shr (8 * $byteIndex)) -band 0xFF)
                if ($actualBytes.ContainsKey($byteAddress) -and $actualBytes[$byteAddress] -ne $byteValue) {
                    Stop-WithOpenOcdFailure "mdw returned conflicting values at 0x$('{0:x}' -f $byteAddress)" $outputLines.ToArray()
                }
                $actualBytes[$byteAddress] = $byteValue
            }
            $actualWords[$wordAddress] = $true
            $wordAddress += 4
        }
    }

    $missingWords = 0
    foreach ($wordAddress in $requestedWords.Keys) {
        if (-not $actualWords.ContainsKey([long]$wordAddress)) { $missingWords++ }
    }
    if (-not $sawMdwLine -or $missingWords -gt 0) {
        $reason = "mdw output was unparsable or contained $($actualWords.Count) of $($requestedWords.Count) requested words"
        if ($openOcdExitCode -ne 0) { $reason += "; process exited with code $openOcdExitCode" }
        Stop-WithOpenOcdFailure $reason $outputLines.ToArray()
    }

    $warningLines = @($outputLines | Where-Object { $_ -match '(?i)^\s*(?:error|fatal)\s*:' })
    foreach ($line in $warningLines) {
        [Console]::Out.WriteLine("rram tail check: WARNING OpenOCD: $line")
    }
    if ($openOcdExitCode -ne 0) {
        [Console]::Out.WriteLine("rram tail check: WARNING OpenOCD exited with code $openOcdExitCode after returning all requested words")
    }

    $lockupText = $outputLines -join "`n"
    $isLockup = $lockupText -match '(?i)clearing lockup after double fault|handler hardfault|\bpc\s*(?::|=)?\s*0x?effffffe'
    [long[]]$compareAddresses = @($compareBytes.Keys | ForEach-Object { [long]$_ })
    [Array]::Sort($compareAddresses)
    $mismatch = $null
    foreach ($address in $compareAddresses) {
        if (-not $actualBytes.ContainsKey($address)) {
            Stop-WithOpenOcdFailure "mdw output did not include compared byte 0x$('{0:x}' -f $address)" $outputLines.ToArray()
        }
        [byte]$expected = $expectedImage.Bytes[[long]$address]
        [byte]$actual = $actualBytes[[long]$address]
        if ($actual -ne $expected) {
            $mismatch = [pscustomobject]@{ Address = [long]$address; Expected = $expected; Actual = $actual }
            break
        }
    }

    if ($null -ne $mismatch) {
        [Console]::Out.WriteLine(("rram tail check: FAIL addr=0x{0:x} first_diff=0x{1:x} expected={2:X2} got={3:X2}" -f $compareAddresses[0], $mismatch.Address, $mismatch.Expected, $mismatch.Actual))
    } else {
        [Console]::Out.WriteLine(("rram tail check: PASS addr=0x{0:x} bytes={1} runs={2}" -f $compareAddresses[0], $compareAddresses.Length, $runs.Count))
    }
    if ($isLockup) {
        [Console]::Out.WriteLine("rram tail check: WARNING target is in LOCKUP after reset (firmware crashed at boot)")
    }
    if ($null -ne $mismatch) { exit 1 }
    if ($isLockup) { exit 3 }
    exit 0
} catch {
    [Console]::Error.WriteLine("rram tail check: FAIL $($_.Exception.Message)")
    exit 1
}
