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
    [long]$highestEnd = 0
    $lastHighestData = $null

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
                if ($count -gt 0) {
                    $hasData = $true
                    if ($recordEnd -gt $highestEnd -or $lastHighestData -eq $null) {
                        $highestEnd = $recordEnd
                        $lastHighestData = $recordInfo
                    } elseif ($recordEnd -eq $highestEnd) {
                        $lastHighestData = $recordInfo
                    }
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
    if (-not $hasData -or $lastHighestData -eq $null) {
        throw "Intel HEX contains no non-empty data records: $InputHex"
    }

    $padding = (0 - $highestEnd) % $Alignment
    if ($padding -lt 0) {
        $padding += $Alignment
    }

    $insertedLines = New-Object 'System.Collections.Generic.List[string]'
    $extensionChanged = $false
    if ($padding -gt 0) {
        [long]$cursor = $highestEnd
        [long]$paddingBase = $lastHighestData.Base
        $paddingBaseType = [int]$lastHighestData.BaseType
        $remaining = $padding

        while ($remaining -gt 0) {
            $offset = $cursor - $paddingBase
            if ($offset -lt 0 -or $offset -gt 65535) {
                if ($paddingBaseType -eq 2) {
                    $nextSegment = ($paddingBase -shr 4) + 4096
                    if ($nextSegment -le 65535) {
                        $paddingBase += 65536
                        $segmentData = New-Object byte[] 2
                        $segmentData[0] = [byte](([int]($nextSegment -shr 8)) -band 0xFF)
                        $segmentData[1] = [byte]([int]$nextSegment -band 0xFF)
                        [void]$insertedLines.Add((New-IHexRecord 2 0 $segmentData))
                        $paddingBaseType = 2
                    } else {
                        $paddingBase = [long]([Math]::Floor($cursor / 65536) * 65536)
                        [void]$insertedLines.Add((New-ExtendedLinearRecord $paddingBase))
                        $paddingBaseType = 4
                    }
                } else {
                    $paddingBase += 65536
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
                    $nextSegment = ($paddingBase -shr 4) + 4096
                    if ($nextSegment -le 65535) {
                        $paddingBase += 65536
                        $segmentData = New-Object byte[] 2
                        $segmentData[0] = [byte](([int]($nextSegment -shr 8)) -band 0xFF)
                        $segmentData[1] = [byte]([int]$nextSegment -band 0xFF)
                        [void]$insertedLines.Add((New-IHexRecord 2 0 $segmentData))
                        $paddingBaseType = 2
                    } else {
                        $paddingBase = [long]([Math]::Floor($cursor / 65536) * 65536)
                        [void]$insertedLines.Add((New-ExtendedLinearRecord $paddingBase))
                        $paddingBaseType = 4
                    }
                } else {
                    $paddingBase += 65536
                    [void]$insertedLines.Add((New-ExtendedLinearRecord $paddingBase))
                    $paddingBaseType = 4
                }
                $extensionChanged = $true
            }
        }

        if ($extensionChanged) {
            $restoreBase = $false
            for ($tailIndex = $lastHighestData.Index + 1; $tailIndex -lt $parsedRecords.Count; $tailIndex++) {
                $tailType = $parsedRecords[$tailIndex].Type
                if ($tailType -eq 2 -or $tailType -eq 4) {
                    break
                }
                if ($tailType -eq 0) {
                    $restoreBase = $true
                    break
                }
                if ($tailType -eq 1) {
                    break
                }
            }

            if ($restoreBase) {
                if ($lastHighestData.BaseType -eq 2) {
                    [void]$insertedLines.Add((New-SegmentBaseRecord $lastHighestData.Base))
                } elseif ($lastHighestData.BaseType -eq 4) {
                    [void]$insertedLines.Add((New-ExtendedLinearRecord $lastHighestData.Base))
                } else {
                    [void]$insertedLines.Add((New-ExtendedLinearRecord 0))
                }
            }
        }
    }

    $outputLines = New-Object 'System.Collections.Generic.List[string]'
    for ($lineIndex = 0; $lineIndex -lt $lines.Count; $lineIndex++) {
        [void]$outputLines.Add($lines[$lineIndex])
        if ($lineIndex -eq $lastHighestData.Index) {
            foreach ($insertedLine in $insertedLines) {
                [void]$outputLines.Add($insertedLine)
            }
        }
    }

    $outputText = ($outputLines -join "`n") + "`n"
    [System.IO.File]::WriteAllText($OutputHex, $outputText, [System.Text.Encoding]::ASCII)
    Write-Output ("hex tail pad: end=0x{0:x} pad={1} bytes" -f $highestEnd, $padding)
    exit 0
} catch {
    [Console]::Error.WriteLine("pad_hex_rram_line: {0}" -f $_.Exception.Message)
    exit 1
}
