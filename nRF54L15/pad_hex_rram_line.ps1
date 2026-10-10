[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$InputHex,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputHex,

    [ValidateRange(1, 65536)]
    [int]$Alignment = 16
)

$ErrorActionPreference = "Stop"

function New-IHexRecord([int]$RecordType, [int]$Address, [byte[]]$Data) {
    $count = $Data.Length
    $sum = $count + (($Address -shr 8) -band 0xFF) + ($Address -band 0xFF) + $RecordType
    $body = "{0:X2}{1:X4}{2:X2}" -f $count, $Address, $RecordType
    foreach ($value in $Data) {
        $sum += [int]$value
        $body += "{0:X2}" -f $value
    }

    $checksum = (256 - ($sum % 256)) % 256
    return (":" + $body + ("{0:X2}" -f $checksum))
}

function New-ExtendedLinearRecord([long]$Base) {
    if ($Base -lt 0 -or $Base -gt 4294901760 -or ($Base % 65536) -ne 0) {
        throw "Cannot encode extended linear base 0x$('{0:x}' -f $Base)."
    }

    $upper = [int]($Base -shr 16)
    $data = New-Object byte[] 2
    $data[0] = [byte](($upper -shr 8) -band 0xFF)
    $data[1] = [byte]($upper -band 0xFF)
    return (New-IHexRecord 4 0 $data)
}

function New-SegmentBaseRecord([long]$Base) {
    if ($Base -lt 0 -or ($Base % 16) -ne 0 -or ($Base -shr 4) -gt 65535) {
        throw "Cannot encode extended segment base 0x$('{0:x}' -f $Base)."
    }

    $segment = [int]($Base -shr 4)
    $data = New-Object byte[] 2
    $data[0] = [byte](($segment -shr 8) -band 0xFF)
    $data[1] = [byte]($segment -band 0xFF)
    return (New-IHexRecord 2 0 $data)
}

try {
    if (-not (Test-Path -LiteralPath $InputHex -PathType Leaf)) {
        throw "Input Intel HEX file not found: $InputHex"
    }

    $rawBytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $InputHex).Path)
    foreach ($value in $rawBytes) {
        if ($value -gt 0x7F) {
            throw "Input is not plain ASCII Intel HEX: $InputHex"
        }
    }

    $text = [System.Text.Encoding]::ASCII.GetString($rawBytes)
    $splitLines = [System.Text.RegularExpressions.Regex]::Split($text, "`r`n|`n|`r")
    $lineCount = $splitLines.Length
    if ($text.EndsWith("`n") -or $text.EndsWith("`r")) {
        $lineCount--
    }
    if ($lineCount -le 0) {
        throw "Input Intel HEX is empty or missing its EOF record: $InputHex"
    }

    $lines = New-Object 'System.Collections.Generic.List[string]'
    for ($lineIndex = 0; $lineIndex -lt $lineCount; $lineIndex++) {
        [void]$lines.Add($splitLines[$lineIndex])
    }

    $parsedRecords = New-Object 'System.Collections.Generic.List[object]'
    [long]$activeBase = 0
    $activeBaseType = 0
    $sawEof = $false
    $hasData = $false

    for ($lineIndex = 0; $lineIndex -lt $lines.Count; $lineIndex++) {
        $lineNumber = $lineIndex + 1
        $line = $lines[$lineIndex]
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
        $data = New-Object 'System.Collections.Generic.List[byte]'
        for ($byteIndex = 0; $byteIndex -lt $count; $byteIndex++) {
            $value = [Convert]::ToInt32($dataHex.Substring(2 * $byteIndex, 2), 16)
            [void]$data.Add([byte]$value)
            $sum += $value
        }
        if (($sum -band 0xFF) -ne 0) {
            throw "Intel HEX checksum is invalid at line $lineNumber."
        }

        if ($sawEof) {
            throw "Intel HEX contains a record after EOF at line $lineNumber."
        }

        $recordInfo = [pscustomobject]@{
            Index = $lineIndex
            Type = $recordType
            Address = $address
            Base = $activeBase
            BaseType = $activeBaseType
            Start = $null
            End = $null
        }

        switch ($recordType) {
            0 {
                if (($address + $count) -gt 65536) {
                    throw "Data record crosses a 64 KiB offset boundary at line $lineNumber."
                }
                $recordEnd = $activeBase + $address + $count
                if ($recordEnd -gt 4294967296) {
                    throw "Data record exceeds the Intel HEX address range at line $lineNumber."
                }
                $recordInfo.End = $recordEnd
                $recordInfo.Start = $activeBase + $address
                if ($count -gt 0) {
                    $hasData = $true
                }
            }
            1 {
                if ($count -ne 0 -or $address -ne 0) {
                    throw "Malformed EOF record at line $lineNumber."
                }
                if ($lineIndex -ne ($lines.Count - 1)) {
                    throw "Intel HEX EOF record must be the final record (line $lineNumber)."
                }
                $sawEof = $true
            }
            2 {
                if ($count -ne 2 -or $address -ne 0) {
                    throw "Malformed extended segment address record at line $lineNumber."
                }
                $segment = [Convert]::ToInt32($dataHex, 16)
                $activeBase = [long]$segment * 16
                $activeBaseType = 2
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
                $upper = [Convert]::ToInt32($dataHex, 16)
                $activeBase = [long]$upper * 65536
                $activeBaseType = 4
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

        [void]$parsedRecords.Add($recordInfo)
    }

    if (-not $sawEof) {
        throw "Intel HEX is missing its EOF record: $InputHex"
    }
    if (-not $hasData) {
        throw "Intel HEX contains no non-empty data records: $InputHex"
    }

    $sortedIntervals = @(
        $parsedRecords |
            Where-Object { $_.Type -eq 0 -and $_.End -gt $_.Start } |
            Sort-Object -Property Start, End
    )
    $runs = New-Object 'System.Collections.Generic.List[object]'
    $currentRun = $null
    foreach ($interval in $sortedIntervals) {
        if ($null -eq $currentRun -or $interval.Start -gt $currentRun.End) {
            if ($null -ne $currentRun) {
                [void]$runs.Add($currentRun)
            }
            $currentRun = [pscustomobject]@{
                Start = [long]$interval.Start
                End = [long]$interval.End
                LastData = $interval
                Padding = 0
            }
        } else {
            if ($interval.End -gt $currentRun.End -or
                ($interval.End -eq $currentRun.End -and $interval.Index -gt $currentRun.LastData.Index)) {
                $currentRun.End = [long]$interval.End
                $currentRun.LastData = $interval
            }
        }
    }
    if ($null -ne $currentRun) {
        [void]$runs.Add($currentRun)
    }

    $insertions = @{}
    [long]$totalPadding = 0
    for ($runIndex = 0; $runIndex -lt $runs.Count; $runIndex++) {
        $run = $runs[$runIndex]
        [long]$alignedEnd = [long]([Math]::Ceiling($run.End / [double]$Alignment) * $Alignment)
        [long]$targetEnd = $alignedEnd
        if ($runIndex + 1 -lt $runs.Count -and $runs[$runIndex + 1].Start -lt $targetEnd) {
            $targetEnd = [long]$runs[$runIndex + 1].Start
        }
        [long]$padding = $targetEnd - $run.End
        if ($padding -le 0) {
            continue
        }
        if ($targetEnd -gt 4294967296) {
            throw "Padding after address 0x$('{0:x}' -f $run.End) exceeds the Intel HEX address range."
        }

        $insertedLines = New-Object 'System.Collections.Generic.List[string]'
        [long]$cursor = $run.End
        [long]$paddingBase = $run.LastData.Base
        $paddingBaseType = [int]$run.LastData.BaseType
        $remaining = $padding
        $extensionChanged = $false

        while ($remaining -gt 0) {
            $offset = $cursor - $paddingBase
            if ($offset -lt 0 -or $offset -gt 65535) {
                if ($paddingBaseType -eq 2) {
                    [long]$nextSegmentBase = $paddingBase + 65536
                    if ($nextSegmentBase -le 1048560) {
                        $paddingBase = $nextSegmentBase
                        [void]$insertedLines.Add((New-SegmentBaseRecord $paddingBase))
                    } else {
                        $paddingBase = [long]([Math]::Floor($cursor / 65536) * 65536)
                        [void]$insertedLines.Add((New-ExtendedLinearRecord $paddingBase))
                        $paddingBaseType = 4
                    }
                } elseif ($paddingBaseType -eq 4) {
                    $paddingBase += 65536
                    [void]$insertedLines.Add((New-ExtendedLinearRecord $paddingBase))
                } else {
                    $paddingBase = [long]([Math]::Floor($cursor / 65536) * 65536)
                    [void]$insertedLines.Add((New-ExtendedLinearRecord $paddingBase))
                    $paddingBaseType = 4
                }
                $extensionChanged = $true
                $offset = $cursor - $paddingBase
                if ($offset -lt 0 -or $offset -gt 65535) {
                    throw "Unable to express padding address 0x$('{0:x}' -f $cursor) with an Intel HEX base record."
                }
            }

            $untilBoundary = 65536 - $offset
            $chunk = [int][Math]::Min([Math]::Min([long]$remaining, [long]$untilBoundary), 255L)
            if ($chunk -le 0) {
                throw "Unable to split padding at address 0x$('{0:x}' -f $cursor)."
            }

            $padData = New-Object byte[] $chunk
            for ($padIndex = 0; $padIndex -lt $chunk; $padIndex++) {
                $padData[$padIndex] = 0xFF
            }
            [void]$insertedLines.Add((New-IHexRecord 0 ([int]$offset) $padData))
            $cursor += $chunk
            $remaining -= $chunk

            if ($remaining -gt 0 -and ($cursor - $paddingBase) -eq 65536) {
                if ($paddingBaseType -eq 2) {
                    [long]$nextSegmentBase = $paddingBase + 65536
                    if ($nextSegmentBase -le 1048560) {
                        $paddingBase = $nextSegmentBase
                        [void]$insertedLines.Add((New-SegmentBaseRecord $paddingBase))
                    } else {
                        $paddingBase = [long]([Math]::Floor($cursor / 65536) * 65536)
                        [void]$insertedLines.Add((New-ExtendedLinearRecord $paddingBase))
                        $paddingBaseType = 4
                    }
                } elseif ($paddingBaseType -eq 4) {
                    $paddingBase += 65536
                    [void]$insertedLines.Add((New-ExtendedLinearRecord $paddingBase))
                } else {
                    $paddingBase = [long]([Math]::Floor($cursor / 65536) * 65536)
                    [void]$insertedLines.Add((New-ExtendedLinearRecord $paddingBase))
                    $paddingBaseType = 4
                }
                $extensionChanged = $true
            }
        }

        if ($extensionChanged) {
            $restoreBase = $false
            for ($tailIndex = $run.LastData.Index + 1; $tailIndex -lt $parsedRecords.Count; $tailIndex++) {
                $tailType = $parsedRecords[$tailIndex].Type
                if ($tailType -eq 2 -or $tailType -eq 4) {
                    break
                }
                if ($tailType -eq 0 -and $parsedRecords[$tailIndex].End -gt $parsedRecords[$tailIndex].Start) {
                    $restoreBase = $true
                    break
                }
                if ($tailType -eq 1) {
                    break
                }
            }
            if ($restoreBase) {
                if ($run.LastData.BaseType -eq 2) {
                    [void]$insertedLines.Add((New-SegmentBaseRecord $run.LastData.Base))
                } elseif ($run.LastData.BaseType -eq 4) {
                    [void]$insertedLines.Add((New-ExtendedLinearRecord $run.LastData.Base))
                } else {
                    [void]$insertedLines.Add((New-ExtendedLinearRecord 0))
                }
            }
        }

        if (-not $insertions.ContainsKey($run.LastData.Index)) {
            $insertions[$run.LastData.Index] = New-Object 'System.Collections.Generic.List[string]'
        }
        foreach ($insertedLine in $insertedLines) {
            [void]$insertions[$run.LastData.Index].Add($insertedLine)
        }
        $run.Padding = $padding
        $totalPadding += $padding
    }

    $highestRun = $runs[$runs.Count - 1]
    $highestPadding = [long]$highestRun.Padding

    $outputLines = New-Object 'System.Collections.Generic.List[string]'
    for ($lineIndex = 0; $lineIndex -lt $lines.Count; $lineIndex++) {
        [void]$outputLines.Add($lines[$lineIndex])
        if ($insertions.ContainsKey($lineIndex)) {
            foreach ($insertedLine in $insertions[$lineIndex]) {
                [void]$outputLines.Add($insertedLine)
            }
        }
    }

    $outputText = ($outputLines -join "`n") + "`n"
    [System.IO.File]::WriteAllText($OutputHex, $outputText, [System.Text.Encoding]::ASCII)
    Write-Output ("hex tail pad: end=0x{0:x} pad={1} bytes" -f $highestRun.End, $highestPadding)
    foreach ($run in $runs) {
        if ($run.Padding -gt 0) {
            Write-Output ("hex run pad: run=0x{0:x}..0x{1:x} pad={2} bytes" -f $run.Start, $run.End, $run.Padding)
        }
    }
    Write-Output ("hex total pad: bytes={0}" -f $totalPadding)
    exit 0
} catch {
    [Console]::Error.WriteLine("pad_hex_rram_line: {0}" -f $_.Exception.Message)
    exit 1
}
