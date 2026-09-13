Set-StrictMode -Version Latest


function Get-MaxUInt64Value {
    param(
        [object[]]$Items,
        [string]$Property
    )

    if ($null -eq $Items -or $Items.Count -eq 0) {
        return $null
    }

    [uint64]$maximum = 0
    foreach ($item in $Items) {
        [uint64]$value = $item.$Property
        if ($value -gt $maximum) {
            $maximum = $value
        }
    }
    return $maximum
}


function Get-BootResponsivenessTelemetry {
    param([string]$Content)

    $interactive = @()
    foreach ($match in [regex]::Matches(
        $Content,
        '(?im)BOOT_TIMING:\s*interactive_ready=(?<ms>\d+)\s*ms'
    )) {
        $interactive += [pscustomobject]@{
            ElapsedMs = [uint64]$match.Groups['ms'].Value
        }
    }

    $startupTotal = @()
    foreach ($match in [regex]::Matches(
        $Content,
        '(?im)BOOT_TIMING:\s*startup_total=(?<ms>\d+)\s*ms'
    )) {
        $startupTotal += [pscustomobject]@{
            ElapsedMs = [uint64]$match.Groups['ms'].Value
        }
    }

    $recovery = @()
    foreach ($match in [regex]::Matches(
        $Content,
        '(?im)OBJECT_RECOVERY_PROGRESS\s+object=(?<object>OBJ_\d{4})\s+index=(?<index>\d+)\s+total=(?<total>\d+)\s+removed=(?<removed>[01])\s+elapsed_ms=(?<ms>\d+)'
    )) {
        $recovery += [pscustomobject]@{
            ObjectId = $match.Groups['object'].Value
            Index = [uint64]$match.Groups['index'].Value
            Total = [uint64]$match.Groups['total'].Value
            Removed = [int]$match.Groups['removed'].Value
            ElapsedMs = [uint64]$match.Groups['ms'].Value
        }
    }

    $dispatch = @()
    foreach ($match in [regex]::Matches(
        $Content,
        '(?im)KEYPAD_EVENT_DISPATCH\s+key=(?<key>-?\d+)\s+age_ms=(?<age>\d+)'
    )) {
        $dispatch += [pscustomobject]@{
            Key = [int]$match.Groups['key'].Value
            AgeMs = [uint64]$match.Groups['age'].Value
        }
    }

    $stale = @()
    foreach ($match in [regex]::Matches(
        $Content,
        '(?im)Dropped stale keypad navigation\s+age_ms=(?<age>\d+)\s+total=(?<total>\d+)'
    )) {
        $stale += [pscustomobject]@{
            AgeMs = [uint64]$match.Groups['age'].Value
            Total = [uint64]$match.Groups['total'].Value
        }
    }

    $priorities = @()
    foreach ($match in [regex]::Matches(
        $Content,
        '(?im)RUNTIME_TASK\s+rank=\d+\s+name=web_status_cach(?:e)?\s+core=(?<core>-?\d+)\s+priority_current=(?<current>\d+)\s+priority_base=(?<base>\d+)\s+inherited=(?<inherited>[01])'
    )) {
        $priorities += [pscustomobject]@{
            Core = [int]$match.Groups['core'].Value
            Current = [uint64]$match.Groups['current'].Value
            Base = [uint64]$match.Groups['base'].Value
            Inherited = [int]$match.Groups['inherited'].Value
        }
    }

    $gateArmedPosition = $Content.IndexOf('KEYPAD_EVENT_GATE state=ARMED', [StringComparison]::OrdinalIgnoreCase)
    $interactivePosition = $Content.IndexOf('BOOT_TIMING: interactive_ready=', [StringComparison]::OrdinalIgnoreCase)
    $startupPosition = $Content.IndexOf('BOOT_TIMING: startup_total=', [StringComparison]::OrdinalIgnoreCase)
    $recoveryStartPosition = $Content.IndexOf('OBJECT_RECOVERY_BACKGROUND state=START', [StringComparison]::OrdinalIgnoreCase)
    $recoveryStopPosition = $Content.IndexOf('OBJECT_RECOVERY_BACKGROUND state=STOP', [StringComparison]::OrdinalIgnoreCase)
    $cacheDonePosition = $Content.IndexOf('WEB_STATUS_CACHE refresh=done', [StringComparison]::OrdinalIgnoreCase)
    $failureLinePattern = '(?im)^[^\r\n]*(?:Guru Meditation Error|panic''ed|Task watchdog got triggered|Brownout detector was triggered|assert failed)[^\r\n]*$'

    return [pscustomobject]@{
        # Startup remains the backward-compatible property consumed by the
        # runner, but now means the actual local-interaction readiness metric.
        Startup = $interactive
        InteractiveReady = $interactive
        StartupTotal = $startupTotal
        Recovery = $recovery
        Dispatch = $dispatch
        Stale = $stale
        Priorities = $priorities
        GateArmedPosition = $gateArmedPosition
        InteractivePosition = $interactivePosition
        StartupPosition = $startupPosition
        RecoveryStartPosition = $recoveryStartPosition
        RecoveryStopPosition = $recoveryStopPosition
        CacheDonePosition = $cacheDonePosition
        SafeShutdownCount = [regex]::Matches($Content, '(?im)Safe shutdown started').Count
        PanicCount = [regex]::Matches($Content, $failureLinePattern).Count
    }
}


function Test-BootResponsivenessTelemetry {
    param(
        [string]$Content,
        # Backward-compatible parameter name. This limit now applies to
        # BOOT_TIMING interactive_ready, not full startup_total.
        [uint64]$MaxStartupMs = 5000,
        [uint64]$MaxRecoveryWindowMs = 250,
        [int]$MinimumRecoverySamples = 1,
        [int]$MinimumKeypadDispatches = 5,
        [uint64]$ExpectedWebCacheBasePriority = 1
    )

    $telemetry = Get-BootResponsivenessTelemetry $Content
    $interactive = @($telemetry.InteractiveReady)
    $startupTotal = @($telemetry.StartupTotal)
    $recovery = @($telemetry.Recovery)
    $dispatch = @($telemetry.Dispatch)
    $stale = @($telemetry.Stale)
    $priorities = @($telemetry.Priorities)

    $missing = @()
    $violations = @()

    if ($interactive.Count -lt 1) {
        $missing += 'BOOT_TIMING interactive_ready'
    }
    if ($startupTotal.Count -lt 1) {
        $missing += 'BOOT_TIMING startup_total'
    }
    if ($telemetry.GateArmedPosition -lt 0) {
        $missing += 'KEYPAD_EVENT_GATE state=ARMED'
    }
    if ($recovery.Count -lt $MinimumRecoverySamples) {
        $missing += "OBJECT_RECOVERY_PROGRESS ($($recovery.Count)/$MinimumRecoverySamples)"
    }
    if ($dispatch.Count -lt $MinimumKeypadDispatches) {
        $missing += "KEYPAD_EVENT_DISPATCH ($($dispatch.Count)/$MinimumKeypadDispatches)"
    }
    if ($telemetry.RecoveryStartPosition -lt 0) {
        $missing += 'OBJECT_RECOVERY_BACKGROUND state=START'
    }
    if ($telemetry.RecoveryStopPosition -lt 0) {
        $missing += 'OBJECT_RECOVERY_BACKGROUND state=STOP'
    }
    if ($telemetry.CacheDonePosition -lt 0) {
        $missing += 'WEB_STATUS_CACHE refresh=done'
    }

    $expectedKeyCodes = @(1, 2, 3, 4, 5)
    $seenKeyCodes = @(
        $dispatch |
            ForEach-Object { [int]$_.Key } |
            Sort-Object -Unique
    )
    $missingKeyCodes = @($expectedKeyCodes | Where-Object { $_ -notin $seenKeyCodes })
    $unexpectedKeyCodes = @($seenKeyCodes | Where-Object { $_ -notin $expectedKeyCodes })
    if ($missingKeyCodes.Count -gt 0) {
        $missing += "KEYPAD_EXPECTED_KEYS missing=$($missingKeyCodes -join ',')"
    }
    if ($unexpectedKeyCodes.Count -gt 0) {
        $violations += "keypad unexpected key codes=$($unexpectedKeyCodes -join ',')"
    }

    $maxInteractive = Get-MaxUInt64Value $interactive 'ElapsedMs'
    $maxStartupTotal = Get-MaxUInt64Value $startupTotal 'ElapsedMs'
    $maxRecovery = Get-MaxUInt64Value $recovery 'ElapsedMs'
    $maxDispatchAge = Get-MaxUInt64Value $dispatch 'AgeMs'

    if ($null -ne $maxInteractive -and $maxInteractive -gt $MaxStartupMs) {
        $violations += "interactive_ready=$($maxInteractive)ms exceeds limit=$($MaxStartupMs)ms"
    }
    if (
        $null -ne $maxInteractive -and
        $null -ne $maxStartupTotal -and
        $maxInteractive -gt $maxStartupTotal
    ) {
        $violations += "interactive_ready=$($maxInteractive)ms occurs after startup_total=$($maxStartupTotal)ms"
    }
    if ($null -ne $maxRecovery -and $maxRecovery -gt $MaxRecoveryWindowMs) {
        $violations += "recovery_window=$($maxRecovery)ms exceeds limit=$($MaxRecoveryWindowMs)ms"
    }
    if ($stale.Count -gt 0) {
        $violations += "stale keypad drops observed count=$($stale.Count) max_age_ms=$(Get-MaxUInt64Value $stale 'AgeMs')"
    }
    if ($telemetry.SafeShutdownCount -gt 0) {
        $violations += "unexpected safe shutdown observed count=$($telemetry.SafeShutdownCount)"
    }
    if ($telemetry.PanicCount -gt 0) {
        $violations += "panic/watchdog/brownout/assert marker observed count=$($telemetry.PanicCount)"
    }

    if (
        $telemetry.GateArmedPosition -ge 0 -and
        $telemetry.InteractivePosition -ge 0 -and
        $telemetry.InteractivePosition -lt $telemetry.GateArmedPosition
    ) {
        $violations += 'interactive_ready logged before keypad gate was armed'
    }
    if (
        $telemetry.InteractivePosition -ge 0 -and
        $telemetry.StartupPosition -ge 0 -and
        $telemetry.StartupPosition -lt $telemetry.InteractivePosition
    ) {
        $violations += 'startup_total logged before interactive_ready'
    }
    if (
        $telemetry.StartupPosition -ge 0 -and
        $telemetry.RecoveryStartPosition -ge 0 -and
        $telemetry.RecoveryStartPosition -lt $telemetry.StartupPosition
    ) {
        $violations += 'background recovery started before startup_total was logged'
    }
    if (
        $telemetry.RecoveryStopPosition -ge 0 -and
        $telemetry.CacheDonePosition -ge 0 -and
        $telemetry.CacheDonePosition -lt $telemetry.RecoveryStopPosition
    ) {
        $violations += 'Web status cache completed before background recovery stopped'
    }

    $inheritedObserved = $false
    foreach ($sample in $priorities) {
        if ($sample.Base -ne $ExpectedWebCacheBasePriority) {
            $violations += "web_status_cache priority_base=$($sample.Base) expected=$ExpectedWebCacheBasePriority"
        }
        $expectedInherited = if ($sample.Current -gt $sample.Base) { 1 } else { 0 }
        if ($sample.Inherited -ne $expectedInherited) {
            $violations += "web_status_cache inherited=$($sample.Inherited) inconsistent with current=$($sample.Current) base=$($sample.Base)"
        }
        if ($sample.Inherited -eq 1) {
            $inheritedObserved = $true
        }
    }

    $removedCount = @($recovery | Where-Object { $_.Removed -ne 0 }).Count

    return [pscustomobject]@{
        Missing = $missing
        Violations = $violations
        # Backward-compatible aliases used by the existing runner.
        StartupCount = $interactive.Count
        MaxStartupMs = $maxInteractive
        InteractiveReadyCount = $interactive.Count
        StartupTotalCount = $startupTotal.Count
        MaxInteractiveReadyMs = $maxInteractive
        MaxStartupTotalMs = $maxStartupTotal
        RecoveryCount = $recovery.Count
        KeypadDispatchCount = $dispatch.Count
        KeypadDistinctKeyCount = $seenKeyCodes.Count
        SeenKeyCodes = $seenKeyCodes
        MissingKeyCodes = $missingKeyCodes
        StaleDropCount = $stale.Count
        PrioritySampleCount = $priorities.Count
        SafeShutdownCount = $telemetry.SafeShutdownCount
        PanicCount = $telemetry.PanicCount
        RemovedObjectCount = $removedCount
        MaxRecoveryWindowMs = $maxRecovery
        MaxKeypadDispatchAgeMs = $maxDispatchAge
        InheritedObserved = $inheritedObserved
    }
}
