[CmdletBinding()]
param(
    [string]$BaseUrl,

    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 60,

    [ValidateRange(100, 60000)]
    [int]$PollIntervalMs = 1000,

    [ValidateRange(1, 60)]
    [int]$RequestTimeoutSeconds = 5,

    [ValidateRange(0, 4294967295)]
    [uint64]$MaxDropCounterDelta = 0,

    [string[]]$ExpectedNodes = @(),
    [string]$OutputDir = "build\hil",
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$script:SelfTestRunning = $false
$script:NetworkCallCount = 0
$script:RawResponses = [System.Collections.Generic.List[object]]::new()
$script:NodeState = @{}

function Get-Field {
    param([AllowNull()][object]$Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Add-Check {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Name,
        [ValidateSet("PASS", "FAIL", "BLOCKED", "NOT_EXPOSED")]
        [string]$Status,
        [string]$Detail
    )
    $Checks.Add([pscustomobject]@{ Name = $Name; Status = $Status; Detail = $Detail }) | Out-Null
}

function Get-RecordKey {
    param([object]$Record)
    return ("{0:X8}-{1:X8}" -f [uint32](Get-Field $Record "boot_id"), [uint32](Get-Field $Record "seq"))
}

function Read-LogPages {
    param(
        [scriptblock]$FetchPage,
        [AllowNull()][string]$AfterToken,
        [ValidateRange(1, 200)][int]$PageLimit,
        [System.Collections.Generic.HashSet[string]]$SeenTokens
    )

    $records = [System.Collections.Generic.List[object]]::new()
    $duplicates = [System.Collections.Generic.List[object]]::new()
    $pages = [System.Collections.Generic.List[object]]::new()
    $batchTokens = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $cursor = $AfterToken

    for ($pageIndex = 0; $pageIndex -lt 64; $pageIndex++) {
        $page = & $FetchPage $cursor $PageLimit
        if ($null -eq $page -or (Get-Field $page "ok") -ne $true) {
            throw "GET /api/nrf/logs returned an invalid or unsuccessful JSON response."
        }
        $pages.Add($page) | Out-Null
        $pageRecords = @((Get-Field $page "records"))
        $pageReset = (Get-Field $page "reset") -eq $true
        foreach ($record in $pageRecords) {
            $key = Get-RecordKey $record
            if (-not $batchTokens.Add($key)) {
                $duplicates.Add($record) | Out-Null
                continue
            }
            if ($SeenTokens.Add($key)) {
                $records.Add($record) | Out-Null
            } elseif (-not $pageReset) {
                $duplicates.Add($record) | Out-Null
            }
        }

        $next = Get-Field $page "next_token"
        if ($pageRecords.Count -lt $PageLimit -or
            [string]::IsNullOrWhiteSpace([string]$next) -or
            [string]$next -eq [string]$cursor) {
            break
        }
        $cursor = [string]$next
        if ($pageIndex -eq 63) { throw "The API did not finish paging within 64 requests." }
    }

    $lastPage = if ($pages.Count -gt 0) { $pages[$pages.Count - 1] } else { $null }
    return [pscustomobject]@{
        Pages = $pages.ToArray()
        Records = $records.ToArray()
        Duplicates = $duplicates.ToArray()
        Cursor = Get-Field $lastPage "next_token"
    }
}

function Invoke-ApiJson {
    param([string]$Uri, [string]$Node, [AllowNull()][string]$AfterToken, [int]$TimeoutSec)
    $script:NetworkCallCount++
    if ($script:SelfTestRunning) { throw "Self-test blocked an attempted network request." }
    $response = Invoke-RestMethod -Uri $Uri -Method Get -TimeoutSec $TimeoutSec -ErrorAction Stop
    $script:RawResponses.Add([pscustomobject]@{
        captured_at_utc = [DateTime]::UtcNow.ToString("o")
        node = $Node
        after = $AfterToken
        response = $response
    }) | Out-Null
    return $response
}

function Get-LiveFetcher {
    param([string]$RootUrl, [string]$Node, [int]$TimeoutSec)
    $fetch = {
        param([AllowNull()][string]$After, [int]$Limit)
        $query = "node=$([uri]::EscapeDataString($Node))&limit=$Limit"
        if (-not [string]::IsNullOrWhiteSpace($After)) {
            $query += "&after=$([uri]::EscapeDataString($After))"
        }
        $uri = "{0}/api/nrf/logs?{1}" -f $RootUrl, $query
        return Invoke-ApiJson $uri $Node $After $TimeoutSec
    }.GetNewClosure()
    return $fetch
}

function Get-ExpectedNodeList {
    param([object]$Status, [string[]]$RequestedNodes)
    $validNames = @("main") + @(0..7 | ForEach-Object { "remote$_" })
    $nodes = [System.Collections.Generic.List[string]]::new()
    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    if (@($RequestedNodes).Count -gt 0) {
        foreach ($node in $RequestedNodes) {
            if ($node -notin $validNames) { throw "Unsupported node name '$node'; use main or remote0 through remote7." }
            if ($seen.Add($node)) { $nodes.Add($node) | Out-Null }
        }
        return $nodes.ToArray()
    }

    $apiNodes = Get-Field $Status "nrf_logs"
    if ($null -eq $apiNodes) { return @() }
    foreach ($node in @($apiNodes)) {
        if ((Get-Field $node "present") -eq $true) {
            $name = [string](Get-Field $node "node")
            if ($name -in $validNames -and $seen.Add($name)) { $nodes.Add($name) | Out-Null }
        }
    }
    return $nodes.ToArray()
}

function Get-ApiNode {
    param([object]$Status, [string]$Name)
    foreach ($node in @((Get-Field $Status "nrf_logs"))) {
        if ([string](Get-Field $node "node") -eq $Name) { return $node }
    }
    return $null
}

function Get-CounterDelta {
    param([uint64]$Before, [uint64]$After)
    if ($After -lt $Before) { return $null }
    return ($After - $Before)
}

function Evaluate-Window {
    param(
        [object]$Status,
        [string[]]$Expected,
        [hashtable]$BaselinePages,
        [hashtable]$FinalPages,
        [hashtable]$NewRecords,
        [hashtable]$DuplicateRecords,
        [uint64]$DropThreshold
    )

    $checks = [System.Collections.Generic.List[object]]::new()
    $expectedNames = @($Expected)
    $missingNodes = [System.Collections.Generic.List[string]]::new()
    if ($expectedNames.Count -eq 0) {
        Add-Check $checks "Connected nRF nodes" "BLOCKED" "/api/status did not identify any present nrf_logs nodes; supply -ExpectedNodes if the expected set is known."
    } else {
        foreach ($name in $expectedNames) {
            $node = Get-ApiNode $Status $name
            if ($null -eq $node -or (Get-Field $node "present") -ne $true) { $missingNodes.Add($name) | Out-Null }
        }
        Add-Check $checks "Connected nRF nodes" $(if ($missingNodes.Count -eq 0) { "PASS" } else { "FAIL" }) `
            $(if ($missingNodes.Count -eq 0) { "Expected nodes are present: $($expectedNames -join ', ')." } else { "Expected nodes missing or not present in /api/status: $($missingNodes -join ', ')." })
    }

    if ($expectedNames.Count -eq 0) {
        Add-Check $checks "Records during window" "BLOCKED" "No expected nodes were available to check."
    } else {
        $counts = [System.Collections.Generic.List[string]]::new()
        $missingRecords = [System.Collections.Generic.List[string]]::new()
        $unsupported = [System.Collections.Generic.List[string]]::new()
        foreach ($name in $expectedNames) {
            $count = @($NewRecords[$name]).Count
            $counts.Add("${name}=$count") | Out-Null
            if ($count -eq 0) {
                $lastPage = $FinalPages[$name]
                if ((Get-Field $lastPage "logs_unsupported") -eq $true) { $unsupported.Add($name) | Out-Null }
                else { $missingRecords.Add($name) | Out-Null }
            }
        }
        $recordStatus = if ($missingRecords.Count -gt 0) { "FAIL" } elseif ($unsupported.Count -gt 0) { "BLOCKED" } else { "PASS" }
        $recordDetail = "New records by node: $($counts -join ', ')."
        if ($unsupported.Count -gt 0) { $recordDetail += " Logs are unsupported on: $($unsupported -join ', ')." }
        if ($missingRecords.Count -gt 0) { $recordDetail += " No new records from: $($missingRecords -join ', ')." }
        Add-Check $checks "Records during window" $recordStatus $recordDetail
    }

    [uint64]$regressions = 0
    [uint64]$duplicates = 0
    [uint64]$gapCount = 0
    [uint64]$bootChanges = 0
    $sequenceDetails = [System.Collections.Generic.List[string]]::new()
    $unexplainedGaps = [System.Collections.Generic.List[string]]::new()
    foreach ($name in $expectedNames) {
        $baseline = $BaselinePages[$name]
        $baselineRecords = @((Get-Field $baseline "records"))
        $lastBoot = $null
        $lastSeq = $null
        [uint64]$nodeRegressions = 0
        [uint64]$nodeDuplicates = 0
        [uint64]$nodeGaps = 0
        [uint64]$nodeBootChanges = 0
        foreach ($record in $baselineRecords) {
            $boot = [uint64](Get-Field $record "boot_id")
            $seq = [uint64](Get-Field $record "seq")
            $eventId = [uint64](Get-Field $record "event_id")
            if ($eventId -eq 65535 -and $seq -eq 4294967295) {
                $lastBoot = $boot
                $lastSeq = $null
            } else {
                $lastBoot = $boot
                $lastSeq = $seq
            }
        }
        foreach ($record in @($NewRecords[$name])) {
            $boot = [uint64](Get-Field $record "boot_id")
            $seq = [uint64](Get-Field $record "seq")
            $eventId = [uint64](Get-Field $record "event_id")
            if ($eventId -eq 65535 -and $seq -eq 4294967295) {
                $bootChanges++
                $nodeBootChanges++
                $lastBoot = $boot
                $lastSeq = $null
                continue
            }
            if ($null -ne $lastBoot -and $boot -ne [uint64]$lastBoot) {
                $bootChanges++
                $nodeBootChanges++
                $lastBoot = $boot
                $lastSeq = $seq
                continue
            }
            if ($null -eq $lastSeq) {
                $lastBoot = $boot
                $lastSeq = $seq
                continue
            }
            if ($seq -lt [uint64]$lastSeq) {
                $regressions++
                $nodeRegressions++
            } elseif ($seq -eq [uint64]$lastSeq) {
                $duplicates++
                $nodeDuplicates++
            } elseif ($seq -gt ([uint64]$lastSeq + 1)) {
                $missing = $seq - [uint64]$lastSeq - 1
                $gapCount += $missing
                $nodeGaps += $missing
                $lastSeq = $seq
            } else {
                $lastSeq = $seq
            }
        }
        $pageDuplicates = @($DuplicateRecords[$name]).Count
        $duplicates += $pageDuplicates
        $nodeDuplicates += $pageDuplicates
        $beforeCounters = Get-Field $BaselinePages[$name] "counters"
        $afterCounters = Get-Field $FinalPages[$name] "counters"
        $beforeBoot = Get-Field $BaselinePages[$name] "boot_id"
        $afterBoot = Get-Field $FinalPages[$name] "boot_id"
        $wireGapDelta = $null
        if ($null -ne $beforeCounters -and $null -ne $afterCounters -and
            $null -ne $beforeBoot -and $null -ne $afterBoot -and
            [uint64]$beforeBoot -eq [uint64]$afterBoot) {
            $beforeWireGap = Get-Field $beforeCounters "wire_gap_total"
            $afterWireGap = Get-Field $afterCounters "wire_gap_total"
            if ($null -ne $beforeWireGap -and $null -ne $afterWireGap) {
                $wireGapDelta = Get-CounterDelta ([uint64]$beforeWireGap) ([uint64]$afterWireGap)
            }
        }
        if ($nodeGaps -gt 0 -and ($null -eq $wireGapDelta -or $wireGapDelta -eq 0)) {
            $unexplainedGaps.Add($name) | Out-Null
        }
        $sequenceDetails.Add(("{0}: regressions={1}, duplicates={2}, missing_seq={3}, boot_id_changes={4}, wire_gap_delta={5}" -f `
            $name, $nodeRegressions, $nodeDuplicates, $nodeGaps, $nodeBootChanges, $(if ($null -eq $wireGapDelta) { "unavailable" } else { $wireGapDelta }))) | Out-Null
    }
    $orderStatus = if ($expectedNames.Count -eq 0) { "BLOCKED" } elseif ($regressions -gt 0 -or $duplicates -gt 0) { "FAIL" } else { "PASS" }
    Add-Check $checks "Sequence regressions and duplicates" $orderStatus `
        ("{0}; totals: regressions={1}, duplicates={2}, boot_id/reboot-marker transitions={3}. A boot_id change starts a new sequence generation." -f `
            ($sequenceDetails -join "; "), $regressions, $duplicates, $bootChanges)

    $gapStatus = if ($expectedNames.Count -eq 0) { "BLOCKED" } elseif ($unexplainedGaps.Count -eq 0) { "PASS" } else { "FAIL" }
    $gapDetail = "Missing sequence values=$gapCount; per-node: $($sequenceDetails -join '; ')."
    if ($unexplainedGaps.Count -gt 0) { $gapDetail += " No wire_gap_total increase for: $($unexplainedGaps -join ', ')." }
    elseif ($gapCount -gt 0) { $gapDetail += " Each observed gap has a same-node wire_gap_total increase; totals are not one-to-one." }
    else { $gapDetail += " No gaps observed." }
    Add-Check $checks "Sequence gaps" $gapStatus $gapDetail

    $counterNames = @("overwritten_total", "lost_internal_total", "wire_gap_total", "local_drop_total")
    $counterDetails = [System.Collections.Generic.List[string]]::new()
    $counterFailures = [System.Collections.Generic.List[string]]::new()
    $counterBlocked = [System.Collections.Generic.List[string]]::new()
    foreach ($name in $expectedNames) {
        $beforePage = $BaselinePages[$name]
        $afterPage = $FinalPages[$name]
        $beforeCounters = Get-Field $beforePage "counters"
        $afterCounters = Get-Field $afterPage "counters"
        $beforeBoot = Get-Field $beforePage "boot_id"
        $afterBoot = Get-Field $afterPage "boot_id"
        if ($null -eq $beforePage -or $null -eq $afterPage -or $null -eq $beforeCounters -or $null -eq $afterCounters) {
            $counterBlocked.Add("$name counters unavailable") | Out-Null
            continue
        }
        if ($null -eq $beforeBoot -or $null -eq $afterBoot -or [uint64]$beforeBoot -ne [uint64]$afterBoot) {
            $counterBlocked.Add("$name boot_id changed or was unknown") | Out-Null
            continue
        }
        foreach ($counter in $counterNames) {
            $oldValue = Get-Field $beforeCounters $counter
            $newValue = Get-Field $afterCounters $counter
            if ($null -eq $oldValue -or $null -eq $newValue) {
                $counterBlocked.Add("$name.$counter unavailable")
                continue
            }
            if ([uint64]$oldValue -eq 4294967295 -or [uint64]$newValue -eq 4294967295) {
                $counterBlocked.Add("$name.$counter saturated at UINT32_MAX") | Out-Null
            }
            $delta = Get-CounterDelta ([uint64]$oldValue) ([uint64]$newValue)
            if ($null -eq $delta) {
                $counterBlocked.Add("$name.$counter decreased without a boot_id change") | Out-Null
                continue
            }
            $counterDetails.Add("$name.$counter=$oldValue->$newValue (delta $delta)") | Out-Null
            if ($delta -gt $DropThreshold) { $counterFailures.Add("$name.$counter delta $delta") | Out-Null }
        }
    }
    if ($expectedNames.Count -eq 0 -or $counterBlocked.Count -gt 0) { $counterStatus = "BLOCKED" }
    else { $counterStatus = "PASS" }
    if ($counterFailures.Count -gt 0) { $counterStatus = "FAIL" }
    $counterDetail = "Per-counter delta threshold=$DropThreshold; $($counterDetails -join '; ')."
    if ($counterFailures.Count -gt 0) { $counterDetail += " Over threshold: $($counterFailures -join ', ')." }
    if ($counterBlocked.Count -gt 0) { $counterDetail += " Blocked: $($counterBlocked -join ', ')." }
    Add-Check $checks "Drop/overflow counter deltas" $counterStatus $counterDetail

    Add-Check $checks "CRC/transport error counters" "NOT_EXPOSED" `
        "The /api/nrf/logs and /api/status schemas expose no CRC or transport-error total; no threshold check can be made."

    return [pscustomobject]@{ Checks = $checks.ToArray(); ExpectedNodes = $expectedNames }
}

function Get-OverallResult {
    param([object[]]$Checks)
    if (@($Checks | Where-Object { $_.Status -eq "FAIL" }).Count -gt 0) { return "FAIL" }
    if (@($Checks | Where-Object { $_.Status -eq "BLOCKED" }).Count -gt 0) { return "BLOCKED" }
    return "PASS"
}

function Assert-SelfTest {
    param([bool]$Condition, [string]$Name)
    if (-not $Condition) { throw "Self-test assertion failed: $Name" }
    Write-Host ("SELFTEST PASS: {0}" -f $Name)
}

function Get-CheckStatus {
    param([object]$Result, [string]$Name)
    foreach ($check in $Result.Checks) {
        if ($check.Name -eq $Name) { return [string]$check.Status }
    }
    return "MISSING"
}

function Invoke-SelfTest {
    $script:SelfTestRunning = $true

    # These fixtures use only fields emitted by web_status_json.c and web_nrf_logs_api.c.
    $statusPass = @'
{"nrf_logs":[
 {"node":"main","channel":null,"present":true,"logs_unsupported":false,"boot_id":101,"stored":2,"warn_count":0,"error_count":0,"fatal_count":0,"wire_gap_total":0,"local_drop_total":0,"overwritten_total":0},
 {"node":"remote0","channel":0,"present":true,"logs_unsupported":false,"boot_id":202,"stored":1,"warn_count":0,"error_count":0,"fatal_count":0,"wire_gap_total":0,"local_drop_total":0,"overwritten_total":0},
 {"node":"remote1","channel":1,"present":false,"logs_unsupported":false,"boot_id":null,"stored":0,"warn_count":0,"error_count":0,"fatal_count":0,"wire_gap_total":0,"local_drop_total":0,"overwritten_total":0}
]}
'@
    $statusMissing = @'
{"nrf_logs":[
 {"node":"main","channel":null,"present":true,"logs_unsupported":false,"boot_id":101,"stored":2,"warn_count":0,"error_count":0,"fatal_count":0,"wire_gap_total":0,"local_drop_total":0,"overwritten_total":0},
 {"node":"remote0","channel":0,"present":false,"logs_unsupported":false,"boot_id":202,"stored":1,"warn_count":0,"error_count":0,"fatal_count":0,"wire_gap_total":0,"local_drop_total":0,"overwritten_total":0}
]}
'@
    $mainBeforeJson = @'
{"ok":true,"node":"main","channel":null,"records":[{"token":"00000065-00000064","boot_id":101,"seq":100,"uptime_ms":1000,"esp_rx_uptime_ms":1100,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0}],"next_token":"00000065-00000064","reset":false,"counters":{"overwritten_total":0,"lost_internal_total":0,"wire_gap_total":0,"local_drop_total":0},"logs_unsupported":false,"boot_id":101}
'@
    $remoteBeforeJson = @'
{"ok":true,"node":"remote0","channel":0,"records":[{"token":"000000CA-0000000A","boot_id":202,"seq":10,"uptime_ms":1000,"esp_rx_uptime_ms":1100,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0}],"next_token":"000000CA-0000000A","reset":false,"counters":{"overwritten_total":0,"lost_internal_total":0,"wire_gap_total":0,"local_drop_total":0},"logs_unsupported":false,"boot_id":202}
'@
    $mainAfterJson = @'
{"ok":true,"node":"main","channel":null,"records":[],"next_token":"00000065-00000064","reset":false,"counters":{"overwritten_total":0,"lost_internal_total":0,"wire_gap_total":1,"local_drop_total":0},"logs_unsupported":false,"boot_id":101}
'@
    $remoteAfterJson = @'
{"ok":true,"node":"remote0","channel":0,"records":[],"next_token":"000000CA-0000000A","reset":false,"counters":{"overwritten_total":0,"lost_internal_total":0,"wire_gap_total":0,"local_drop_total":0},"logs_unsupported":false,"boot_id":202}
'@
    $record101 = '{"token":"00000065-00000065","boot_id":101,"seq":101,"uptime_ms":1100,"esp_rx_uptime_ms":1200,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0}' | ConvertFrom-Json
    $record103 = '{"token":"00000065-00000067","boot_id":101,"seq":103,"uptime_ms":1300,"esp_rx_uptime_ms":1400,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0}' | ConvertFrom-Json
    $record11 = '{"token":"000000CA-0000000B","boot_id":202,"seq":11,"uptime_ms":1100,"esp_rx_uptime_ms":1200,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0}' | ConvertFrom-Json
    $rebootMarker = '{"token":"00000065-FFFFFFFF","boot_id":101,"seq":4294967295,"uptime_ms":1400,"esp_rx_uptime_ms":1500,"event_id":65535,"severity":2,"subsystem":0,"arg0":100,"arg1":101}' | ConvertFrom-Json
    $recordZero = '{"token":"00000065-00000000","boot_id":101,"seq":0,"uptime_ms":10,"esp_rx_uptime_ms":1510,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0}' | ConvertFrom-Json

    $status = $statusPass | ConvertFrom-Json
    $before = @{ main = ($mainBeforeJson | ConvertFrom-Json); remote0 = ($remoteBeforeJson | ConvertFrom-Json) }
    $after = @{ main = ($mainAfterJson | ConvertFrom-Json); remote0 = ($remoteAfterJson | ConvertFrom-Json) }
    $records = @{ main = @($record101, $record103); remote0 = @($record11) }
    $duplicates = @{ main = @(); remote0 = @() }
    $pass = Evaluate-Window $status @("main", "remote0") $before $after $records $duplicates 1
    Assert-SelfTest ((Get-CheckStatus $pass "Connected nRF nodes") -eq "PASS") "present expected nodes pass"
    Assert-SelfTest ((Get-CheckStatus $pass "Records during window") -eq "PASS") "every expected node produced records"
    Assert-SelfTest ((Get-CheckStatus $pass "Sequence regressions and duplicates") -eq "PASS") "strictly increasing sequence values pass"
    Assert-SelfTest ((Get-CheckStatus $pass "Sequence gaps") -eq "PASS") "sequence gap is reported and matched by wire_gap_total"
    Assert-SelfTest ((Get-CheckStatus $pass "Drop/overflow counter deltas") -eq "PASS") "loss counter deltas at threshold pass"
    Assert-SelfTest ((Get-CheckStatus $pass "CRC/transport error counters") -eq "NOT_EXPOSED") "missing CRC/transport fields are informational"
    Assert-SelfTest ((Get-OverallResult $pass.Checks) -eq "PASS") "supported pass scenario passes with unavailable CRC telemetry"

    $markerBaseline = ($mainBeforeJson | ConvertFrom-Json)
    $markerBaseline.records = @($markerBaseline.records[0], $rebootMarker)
    $markerCheck = Evaluate-Window $status @("main") @{ main = $markerBaseline } @{ main = $after.main } `
        @{ main = @($recordZero) } $duplicates 1
    Assert-SelfTest ((Get-CheckStatus $markerCheck "Sequence regressions and duplicates") -eq "PASS") "reserved reboot marker permits the new generation sequence zero"

    $missing = Evaluate-Window ($statusMissing | ConvertFrom-Json) @("main", "remote0") $before $after `
        @{ main = @(); remote0 = @() } $duplicates 1
    Assert-SelfTest ((Get-CheckStatus $missing "Connected nRF nodes") -eq "FAIL") "missing expected node fails presence check"
    Assert-SelfTest ((Get-CheckStatus $missing "Records during window") -eq "FAIL") "connected node without window records fails"

    $regressionRecords = @{ main = @('{"token":"00000065-00000063","boot_id":101,"seq":99,"uptime_ms":900,"esp_rx_uptime_ms":1000,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0}' | ConvertFrom-Json) }
    $regressionCheck = Evaluate-Window $status @("main") @{ main = $before.main } @{ main = $after.main } `
        $regressionRecords $duplicates 1
    Assert-SelfTest ((Get-CheckStatus $regressionCheck "Sequence regressions and duplicates") -eq "FAIL") "sequence regression fails"

    $duplicateRecord = '{"token":"00000065-00000065","boot_id":101,"seq":101,"uptime_ms":1100,"esp_rx_uptime_ms":1200,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0}' | ConvertFrom-Json
    $duplicateCheck = Evaluate-Window $status @("main") @{ main = $before.main } @{ main = $after.main } `
        @{ main = @($record101, $duplicateRecord) } $duplicates 1
    Assert-SelfTest ((Get-CheckStatus $duplicateCheck "Sequence regressions and duplicates") -eq "FAIL") "duplicate sequence fails"

    $unexplained = @{ main = ($mainAfterJson | ConvertFrom-Json); remote0 = $after.remote0 }
    $unexplained.main.counters.wire_gap_total = 0
    $unexplained.remote0.counters.wire_gap_total = 1
    $unexplainedCheck = Evaluate-Window $status @("main", "remote0") $before $unexplained `
        @{ main = @($record101, $record103); remote0 = @($record11) } $duplicates 1
    Assert-SelfTest ((Get-CheckStatus $unexplainedCheck "Sequence gaps") -eq "FAIL") "another node's counter cannot explain this node's gap"

    $overThreshold = @{ main = ($mainAfterJson | ConvertFrom-Json); remote0 = $after.remote0 }
    $overThreshold.main.counters.wire_gap_total = 2
    $dropCheck = Evaluate-Window $status @("main") @{ main = $before.main } $overThreshold `
        @{ main = @($record101) } $duplicates 1
    Assert-SelfTest ((Get-CheckStatus $dropCheck "Drop/overflow counter deltas") -eq "FAIL") "drop counter increase beyond threshold fails"

    $decreasedBefore = ($mainBeforeJson | ConvertFrom-Json)
    $decreasedBefore.counters.local_drop_total = 2
    $decreasedCheck = Evaluate-Window $status @("main") @{ main = $decreasedBefore } @{ main = $after.main } `
        @{ main = @($record101) } $duplicates 1
    Assert-SelfTest ((Get-CheckStatus $decreasedCheck "Drop/overflow counter deltas") -eq "BLOCKED") "decreasing saturating counter is not treated as uint32 wrap"

    $saturatedBefore = ($mainBeforeJson | ConvertFrom-Json)
    $saturatedAfter = ($mainAfterJson | ConvertFrom-Json)
    $saturatedBefore.counters.local_drop_total = 4294967295
    $saturatedAfter.counters.local_drop_total = 4294967295
    $saturatedCheck = Evaluate-Window $status @("main") @{ main = $saturatedBefore } @{ main = $saturatedAfter } `
        @{ main = @($record101) } $duplicates 1
    Assert-SelfTest ((Get-CheckStatus $saturatedCheck "Drop/overflow counter deltas") -eq "BLOCKED") "saturated loss total is not reported as a measurable zero delta"

    # A two-page canned response proves exclusive after-token paging without HTTP.
    $pageOne = @'
{"ok":true,"node":"main","channel":null,"records":[{"token":"00000065-00000064","boot_id":101,"seq":100,"uptime_ms":1000,"esp_rx_uptime_ms":1100,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0}],"next_token":"00000065-00000064","reset":false,"counters":{"overwritten_total":0,"lost_internal_total":0,"wire_gap_total":0,"local_drop_total":0},"logs_unsupported":false,"boot_id":101}
'@ | ConvertFrom-Json
    $pageTwo = @'
{"ok":true,"node":"main","channel":null,"records":[{"token":"00000065-00000065","boot_id":101,"seq":101,"uptime_ms":1100,"esp_rx_uptime_ms":1200,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0}],"next_token":"00000065-00000065","reset":false,"counters":{"overwritten_total":0,"lost_internal_total":0,"wire_gap_total":0,"local_drop_total":0},"logs_unsupported":false,"boot_id":101}
'@ | ConvertFrom-Json
    $pageEnd = @'
{"ok":true,"node":"main","channel":null,"records":[],"next_token":"00000065-00000065","reset":false,"counters":{"overwritten_total":0,"lost_internal_total":0,"wire_gap_total":0,"local_drop_total":0},"logs_unsupported":false,"boot_id":101}
'@ | ConvertFrom-Json
    $mockResponses = @($pageOne, $pageTwo, $pageEnd)
    $mockCalls = [System.Collections.Generic.List[string]]::new()
    $mockFetcher = {
        param([AllowNull()][string]$After, [int]$Limit)
        $mockCalls.Add([string]$After) | Out-Null
        $index = $mockCalls.Count - 1
        return $mockResponses[$index]
    }.GetNewClosure()
    $pageSeen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $paged = Read-LogPages $mockFetcher $null 1 $pageSeen
    Assert-SelfTest ($paged.Records.Count -eq 2 -and $paged.Pages.Count -eq 3) "two-page response drains to the exclusive after cursor"
    Assert-SelfTest ($mockCalls.Count -eq 3 -and $mockCalls[1] -eq "00000065-00000064") "paging continues with next_token as after"

    $resetPage = @'
{"ok":true,"node":"main","channel":null,"records":[{"token":"00000065-00000064","boot_id":101,"seq":100,"uptime_ms":1000,"esp_rx_uptime_ms":1100,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0},{"token":"00000065-00000065","boot_id":101,"seq":101,"uptime_ms":1100,"esp_rx_uptime_ms":1200,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0}],"next_token":"00000065-00000065","reset":true,"counters":{"overwritten_total":0,"lost_internal_total":0,"wire_gap_total":0,"local_drop_total":0},"logs_unsupported":false,"boot_id":101}
'@ | ConvertFrom-Json
    $resetSeen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    [void]$resetSeen.Add("00000065-00000064")
    $resetFetcher = { param($After, $Limit) return $resetPage }.GetNewClosure()
    $resetRead = Read-LogPages $resetFetcher "00000065-000000FF" 200 $resetSeen
    Assert-SelfTest ($resetRead.Records.Count -eq 1 -and $resetRead.Duplicates.Count -eq 0) "reset replay is deduplicated without losing new records"

    $repeatedPage = @'
{"ok":true,"node":"main","channel":null,"records":[{"token":"00000065-00000064","boot_id":101,"seq":100,"uptime_ms":1000,"esp_rx_uptime_ms":1100,"event_id":1,"severity":1,"subsystem":1,"arg0":0,"arg1":0}],"next_token":"00000065-00000064","reset":false,"counters":{"overwritten_total":0,"lost_internal_total":0,"wire_gap_total":0,"local_drop_total":0},"logs_unsupported":false,"boot_id":101}
'@ | ConvertFrom-Json
    $repeatedSeen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    [void]$repeatedSeen.Add("00000065-00000064")
    $repeatedFetcher = { param($After, $Limit) return $repeatedPage }.GetNewClosure()
    $repeatedRead = Read-LogPages $repeatedFetcher "00000065-00000064" 200 $repeatedSeen
    Assert-SelfTest ($repeatedRead.Records.Count -eq 0 -and $repeatedRead.Duplicates.Count -eq 1) "repeated non-reset token is reported as duplicate"

    Assert-SelfTest ($script:NetworkCallCount -eq 0) "self-test made zero network requests"
    Write-Host "CRC_TRANSPORT_COUNTERS: NOT_EXPOSED by current API schema."
    Write-Host "COLLECTOR_BUDGET_BACKOFF: NOT_EXPOSED by current HTTP schema."
    Write-Host "NRF_LOG_TRANSPORT_HIL_SELFTEST_RESULT: PASS"
}

if ($SelfTest) {
    try {
        Invoke-SelfTest
        exit 0
    } catch {
        Write-Host "NRF_LOG_TRANSPORT_HIL_SELFTEST_RESULT: FAIL"
        Write-Host ("reason={0}" -f $_.Exception.Message)
        Write-Host $_.ScriptStackTrace
        exit 1
    }
}

$checks = [System.Collections.Generic.List[object]]::new()
$baselinePages = @{}
$finalPages = @{}
$windowRecords = @{}
$windowDuplicates = @{}
$expected = @()
$outputPath = $null
$capturePath = $null
$summaryPath = $null
$captureStarted = $false
$startTime = [DateTime]::UtcNow

try {
    if ([string]::IsNullOrWhiteSpace($BaseUrl)) { throw "Live mode requires -BaseUrl for the running MAIN board." }
    $rootUrl = $BaseUrl.TrimEnd("/")
    $outputPath = if ([IO.Path]::IsPathRooted($OutputDir)) { [IO.Path]::GetFullPath($OutputDir) } else { [IO.Path]::GetFullPath((Join-Path (Join-Path $PSScriptRoot "..") $OutputDir)) }
    $status = Invoke-ApiJson ("{0}/api/status" -f $rootUrl) "status" $null $RequestTimeoutSeconds
    $expected = @(Get-ExpectedNodeList $status $ExpectedNodes)
    if ($expected.Count -eq 0) {
        Add-Check $checks "Connected nRF nodes" "BLOCKED" "/api/status has no present nrf_logs nodes; pass -ExpectedNodes if an expected set is known."
        Add-Check $checks "Records during window" "BLOCKED" "No expected nodes were available to poll."
    } else {
        $captureStarted = $true
        foreach ($node in $expected) {
            $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            $fetcher = Get-LiveFetcher $rootUrl $node $RequestTimeoutSeconds
            $snapshot = Read-LogPages $fetcher $null 200 $seen
            $baselinePages[$node] = $snapshot.Pages[-1]
            $finalPages[$node] = $snapshot.Pages[-1]
            $windowRecords[$node] = [System.Collections.Generic.List[object]]::new()
            $windowDuplicates[$node] = [System.Collections.Generic.List[object]]::new()
            $script:NodeState[$node] = [pscustomobject]@{ Cursor = $snapshot.Cursor; Seen = $seen; Fetcher = $fetcher }
        }

        $watch = [Diagnostics.Stopwatch]::StartNew()
        while ($true) {
            foreach ($node in $expected) {
                $state = $script:NodeState[$node]
                $batch = Read-LogPages $state.Fetcher $state.Cursor 200 $state.Seen
                foreach ($record in $batch.Records) { $windowRecords[$node].Add($record) | Out-Null }
                foreach ($record in $batch.Duplicates) { $windowDuplicates[$node].Add($record) | Out-Null }
                if ($batch.Pages.Count -gt 0) { $finalPages[$node] = $batch.Pages[-1] }
                if (-not [string]::IsNullOrWhiteSpace([string]$batch.Cursor)) { $state.Cursor = [string]$batch.Cursor }
                $script:NodeState[$node] = $state
            }
            if ($watch.ElapsedMilliseconds -ge ($DurationSeconds * 1000)) { break }
            $remaining = ($DurationSeconds * 1000) - $watch.ElapsedMilliseconds
            if ($remaining -gt 0) { Start-Sleep -Milliseconds ([int][Math]::Min($PollIntervalMs, $remaining)) }
        }

        $evaluated = Evaluate-Window $status $expected $baselinePages $finalPages $windowRecords $windowDuplicates $MaxDropCounterDelta
        foreach ($check in $evaluated.Checks) { $checks.Add($check) | Out-Null }
    }
} catch {
    Add-Check $checks "Runner execution" $(if ($captureStarted) { "FAIL" } else { "BLOCKED" }) $_.Exception.Message
}

if ($null -ne $outputPath) {
    try {
        New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $capturePath = Join-Path $outputPath "nrf-log-transport-$stamp.jsonl"
        $summaryPath = Join-Path $outputPath "nrf-log-transport-$stamp.summary.json"
        [IO.File]::WriteAllLines($capturePath, @($script:RawResponses | ForEach-Object { $_ | ConvertTo-Json -Depth 12 -Compress }))
    } catch {
        Add-Check $checks "HIL result files" "FAIL" $_.Exception.Message
    }
}

$result = Get-OverallResult $checks.ToArray()
$summary = [pscustomobject][ordered]@{
    result = $result
    started_at_utc = $startTime.ToString("o")
    duration_seconds = $DurationSeconds
    poll_interval_ms = $PollIntervalMs
    expected_nodes = @($expected)
    max_drop_counter_delta = $MaxDropCounterDelta
    checks = $checks.ToArray()
    telemetry_availability = [pscustomobject]@{
        crc_transport_error_counters = "NOT_EXPOSED"
        collector_budget_backoff_stats = "NOT_EXPOSED"
    }
    capture_path = $capturePath
}
if (-not [string]::IsNullOrWhiteSpace($summaryPath)) {
    try { [IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 12)) }
    catch {
        Add-Check $checks "HIL result files" "FAIL" $_.Exception.Message
        $result = Get-OverallResult $checks.ToArray()
    }
}

Write-Host "===== nRF log transport HIL report ====="
Write-Host ("{0,-43} {1,-9} {2}" -f "CHECK", "STATUS", "DETAIL")
Write-Host ("{0,-43} {1,-9} {2}" -f "-----", "------", "------")
foreach ($check in $checks) {
    Write-Host ("{0,-43} {1,-9} {2}" -f $check.Name, $check.Status, $check.Detail)
}
Write-Host "CRC_TRANSPORT_COUNTERS: NOT_EXPOSED by /api/nrf/logs and /api/status."
Write-Host "COLLECTOR_BUDGET_BACKOFF: NOT_EXPOSED by /api/nrf/logs and /api/status."
if ($capturePath) { Write-Host ("CAPTURE_PATH: {0}" -f $capturePath) }
if ($summaryPath) { Write-Host ("SUMMARY_PATH: {0}" -f $summaryPath) }
Write-Host "SAFETY: live mode performs GET requests only; it never flashes, writes, or power-cycles the board."
Write-Host ("NRF_LOG_TRANSPORT_HIL_RESULT: {0}" -f $result)

if ($result -eq "PASS") { exit 0 }
if ($result -eq "FAIL") { exit 1 }
exit 2
