# Shared parser for the opt-in keypad timing HIL profile.
# This file only declares functions; callers dot-source it before use.

function Convert-KeypadTimingMatch {
    param(
        [System.Text.RegularExpressions.Match]$Match,
        [string[]]$Fields
    )

    $values = [ordered]@{}
    foreach ($field in $Fields) {
        $values[$field] = if ($field -eq "source") {
            [string]$Match.Groups[$field].Value
        } else {
            [uint64]$Match.Groups[$field].Value
        }
    }
    return [pscustomobject]$values
}


function Test-KeypadTimingMetricShape {
    param(
        [object]$Metric,
        [string]$Kind,
        [System.Collections.Generic.List[string]]$Issues
    )

    $prefix = "{0} timing marker" -f $Kind
    if ([uint64]$Metric.window_ms -eq 0) {
        $Issues.Add("$prefix has window_ms=0.")
    }
    if ([uint64]$Metric.samples -eq 0 -and $Kind -eq "ESP_TIMER_TASK") {
        $Issues.Add("$prefix has samples=0.")
    }
    if ($Kind -eq "ADC" -and [uint64]$Metric.snapshots -eq 0) {
        $Issues.Add("$prefix has snapshots=0.")
    }
    if ($Kind -eq "ADC" -and [uint64]$Metric.errors -ne 0) {
        $Issues.Add("$prefix has errors=$($Metric.errors); non-zero ADC read errors are not acceptable.")
    }

    $count = if ($Kind -eq "ADC") {
        [uint64]$Metric.snapshots
    } else {
        [uint64]$Metric.samples
    }
    if ($count -gt 0) {
        $min = [uint64]$Metric.min_us
        $avg = [uint64]$Metric.avg_us
        $p95 = [uint64]$Metric.p95_us
        $p95Overflow = [uint64]$Metric.p95_overflow
        $p95CensoredAbove = [uint64]$Metric.p95_censored_above_us
        $max = [uint64]$Metric.max_us
        $resolution = [uint64]$Metric.resolution_us

        if ($resolution -eq 0) {
            $Issues.Add("$prefix has resolution_us=0.")
        }
        if ($min -gt $avg -or $avg -gt $max) {
            $Issues.Add("$prefix violates min_us <= avg_us <= max_us.")
        }
        if ($p95Overflow -gt 1) {
            $Issues.Add("$prefix has invalid p95_overflow=$p95Overflow.")
        } elseif ($p95Overflow -eq 1) {
            if ([uint64]$Metric.overflow -eq 0) {
                $Issues.Add("$prefix marks p95 overflow without histogram overflow samples.")
            }
            if ($p95 -ne 0) {
                $Issues.Add("$prefix must set p95_us=0 when the percentile rank is censored.")
            }
            if ($p95CensoredAbove -eq 0) {
                $Issues.Add("$prefix is missing p95_censored_above_us for a censored percentile.")
            } elseif ($max -lt $p95CensoredAbove) {
                $Issues.Add("$prefix max_us is below p95_censored_above_us.")
            }
        } else {
            if ($p95CensoredAbove -ne 0) {
                $Issues.Add("$prefix has p95_censored_above_us without a censored percentile.")
            }
            if ($resolution -gt 0 -and ($p95 -lt $min -or $p95 -gt ($max + $resolution - 1))) {
                $Issues.Add("$prefix p95_us is outside the measured range plus histogram resolution.")
            }
        }
    }

    if ($Kind -eq "ADC" -and [uint64]$Metric.attempts -lt [uint64]$Metric.snapshots) {
        $Issues.Add("$prefix has attempts below snapshots.")
    }
    if ($Kind -eq "ESP_TIMER_TASK" -and [uint64]$Metric.late_count -gt [uint64]$Metric.samples) {
        $Issues.Add("$prefix has late_count above samples.")
    }
    if ($Kind -eq "ESP_TIMER_TASK" -and [uint64]$Metric.overrun_count -gt [uint64]$Metric.late_count) {
        $Issues.Add("$prefix has overrun_count above late_count.")
    }
}


function Format-KeypadTimingAggregate {
    param(
        [object[]]$Metrics,
        [string]$Kind
    )

    if ($Metrics.Count -eq 0) {
        return "No $Kind timing markers were observed."
    }

    $source = if ($Kind -eq "ADC") { [string]$Metrics[0].source } else { "" }
    if ($Kind -eq "ADC") {
        $totalCount = [uint64]0
        $totalAttempts = [uint64]0
        $totalBusy = [uint64]0
        $totalErrors = [uint64]0
        $weightedSum = [decimal]0
        $minimum = [uint64]::MaxValue
        $maximum = [uint64]0
        $worstP95 = [uint64]0
        $p95Overflow = $false
        $p95CensoredAbove = [uint64]0
        $overflow = [uint64]0
        foreach ($metric in $Metrics) {
            $totalCount += [uint64]$metric.snapshots
            $totalAttempts += [uint64]$metric.attempts
            $totalBusy += [uint64]$metric.busy
            $totalErrors += [uint64]$metric.errors
            $weightedSum += [decimal]$metric.avg_us * [uint64]$metric.snapshots
            $minimum = [math]::Min($minimum, [uint64]$metric.min_us)
            $maximum = [math]::Max($maximum, [uint64]$metric.max_us)
            if ([uint64]$metric.p95_overflow -ne 0) {
                $p95Overflow = $true
                $p95CensoredAbove = [math]::Max(
                    $p95CensoredAbove,
                    [uint64]$metric.p95_censored_above_us
                )
            } else {
                $worstP95 = [math]::Max($worstP95, [uint64]$metric.p95_us)
            }
            $overflow += [uint64]$metric.overflow
        }
        $average = if ($totalCount -gt 0) {
            [math]::Round($weightedSum / $totalCount, 2)
        } else {
            0
        }
        $p95Detail = if ($p95Overflow) {
            "p95_us=0 p95_overflow=1 p95_censored_above_us=$p95CensoredAbove"
        } else {
            "p95_us=$worstP95 p95_overflow=0 p95_censored_above_us=0"
        }
        return "source=$source windows=$($Metrics.Count) snapshots=$totalCount attempts=$totalAttempts busy=$totalBusy errors=$totalErrors min_us=$minimum avg_us=$average $p95Detail max_us=$maximum resolution_us=$($Metrics[-1].resolution_us) overflow=$overflow (p95 is the worst reported window when valid; censored when its rank lands above the histogram range)."
    }

    $totalCount = [uint64]0
    $weightedSum = [decimal]0
    $minimum = [uint64]::MaxValue
    $maximum = [uint64]0
    $worstP95 = [uint64]0
    $p95Overflow = $false
    $p95CensoredAbove = [uint64]0
    $late = [uint64]0
    $overrun = [uint64]0
    $overflow = [uint64]0
    $period = [uint64]$Metrics[0].period_us
    foreach ($metric in $Metrics) {
        $totalCount += [uint64]$metric.samples
        $weightedSum += [decimal]$metric.avg_us * [uint64]$metric.samples
        $minimum = [math]::Min($minimum, [uint64]$metric.min_us)
        $maximum = [math]::Max($maximum, [uint64]$metric.max_us)
        if ([uint64]$metric.p95_overflow -ne 0) {
            $p95Overflow = $true
            $p95CensoredAbove = [math]::Max(
                $p95CensoredAbove,
                [uint64]$metric.p95_censored_above_us
            )
        } else {
            $worstP95 = [math]::Max($worstP95, [uint64]$metric.p95_us)
        }
        $late += [uint64]$metric.late_count
        $overrun += [uint64]$metric.overrun_count
        $overflow += [uint64]$metric.overflow
    }
    $average = if ($totalCount -gt 0) {
        [math]::Round($weightedSum / $totalCount, 2)
    } else {
        0
    }
    $p95Detail = if ($p95Overflow) {
        "p95_lateness_us=0 p95_overflow=1 p95_censored_above_us=$p95CensoredAbove"
    } else {
        "p95_lateness_us=$worstP95 p95_overflow=0 p95_censored_above_us=0"
    }
    return "period_us=$period windows=$($Metrics.Count) samples=$totalCount min_lateness_us=$minimum avg_lateness_us=$average $p95Detail max_lateness_us=$maximum late_count=$late overrun_count=$overrun resolution_us=$($Metrics[-1].resolution_us) overflow=$overflow (p95 is the worst reported window when valid; censored when its rank lands above the histogram range)."
}


function Test-KeypadTimingTelemetry {
    param(
        [string]$Content,
        [string]$ExpectedSource,
        [ValidateRange(1, 60)]
        [int]$MinimumWindows = 3,
        [ValidateRange(1, 1000000)]
        [int]$MinimumSnapshots = 100
    )

    $sourceIssues = [System.Collections.Generic.List[string]]::new()
    $adcIssues = [System.Collections.Generic.List[string]]::new()
    $timerIssues = [System.Collections.Generic.List[string]]::new()
    $adcShapeIssues = [System.Collections.Generic.List[string]]::new()
    $timerShapeIssues = [System.Collections.Generic.List[string]]::new()
    $invalidTelemetry = $false

    $readyPattern = "(?im)KEYPAD_TIMING_READY\s+source=(?<source>[A-Z0-9_]+)"
    $adcPattern = "(?im)KEYPAD_ADC_TIMING:\s*source=(?<source>[A-Z0-9_]+)\s+window_ms=(?<window_ms>\d+)\s+attempts=(?<attempts>\d+)\s+snapshots=(?<snapshots>\d+)\s+busy=(?<busy>\d+)\s+errors=(?<errors>\d+)\s+min_us=(?<min_us>\d+)\s+avg_us=(?<avg_us>\d+)\s+p95_us=(?<p95_us>\d+)\s+p95_overflow=(?<p95_overflow>\d+)\s+p95_censored_above_us=(?<p95_censored_above_us>\d+)\s+max_us=(?<max_us>\d+)\s+resolution_us=(?<resolution_us>\d+)\s+overflow=(?<overflow>\d+)"
    $timerPattern = "(?im)KEYPAD_TIMER_TIMING:\s*period_us=(?<period_us>\d+)\s+window_ms=(?<window_ms>\d+)\s+samples=(?<samples>\d+)\s+min_lateness_us=(?<min_us>\d+)\s+avg_lateness_us=(?<avg_us>\d+)\s+p95_lateness_us=(?<p95_us>\d+)\s+p95_overflow=(?<p95_overflow>\d+)\s+p95_censored_above_us=(?<p95_censored_above_us>\d+)\s+max_lateness_us=(?<max_us>\d+)\s+late_count=(?<late_count>\d+)\s+overrun_count=(?<overrun_count>\d+)\s+resolution_us=(?<resolution_us>\d+)\s+overflow=(?<overflow>\d+)"

    $readyMarkers = @([regex]::Matches($Content, $readyPattern))
    $adcMarkers = @([regex]::Matches($Content, $adcPattern))
    $timerMarkers = @([regex]::Matches($Content, $timerPattern))
    $readyKeywordCount = @([regex]::Matches($Content, "(?im)KEYPAD_TIMING_READY")).Count
    $adcKeywordCount = @([regex]::Matches($Content, "(?im)KEYPAD_ADC_TIMING:")).Count
    $timerKeywordCount = @([regex]::Matches($Content, "(?im)KEYPAD_TIMER_TIMING:")).Count

    if ($readyMarkers.Count -eq 0) {
        $sourceIssues.Add("KEYPAD_TIMING_READY was not observed.")
    } elseif ($readyKeywordCount -ne $readyMarkers.Count) {
        $sourceIssues.Add("KEYPAD_TIMING_READY was present but malformed or incomplete.")
        $invalidTelemetry = $true
    }

    $adcFields = @("window_ms", "attempts", "snapshots", "busy", "errors", "min_us", "avg_us", "p95_us", "p95_overflow", "p95_censored_above_us", "max_us", "resolution_us", "overflow")
    $timerFields = @("period_us", "window_ms", "samples", "min_us", "avg_us", "p95_us", "p95_overflow", "p95_censored_above_us", "max_us", "late_count", "overrun_count", "resolution_us", "overflow")
    $adc = @($adcMarkers | ForEach-Object {
        Convert-KeypadTimingMatch -Match $_ -Fields ($adcFields + @("source"))
    })
    $timer = @($timerMarkers | ForEach-Object {
        Convert-KeypadTimingMatch -Match $_ -Fields $timerFields
    })

    if ($adcMarkers.Count -eq 0) {
        $adcIssues.Add("No valid KEYPAD_ADC_TIMING marker was observed.")
        if ($adcKeywordCount -gt 0) {
            $invalidTelemetry = $true
        }
    } elseif ($adcKeywordCount -ne $adcMarkers.Count) {
        $adcIssues.Add("KEYPAD_ADC_TIMING was present but malformed or incomplete.")
        $invalidTelemetry = $true
    }
    if ($timerMarkers.Count -eq 0) {
        $timerIssues.Add("No valid KEYPAD_TIMER_TIMING marker was observed.")
        if ($timerKeywordCount -gt 0) {
            $invalidTelemetry = $true
        }
    } elseif ($timerKeywordCount -ne $timerMarkers.Count) {
        $timerIssues.Add("KEYPAD_TIMER_TIMING was present but malformed or incomplete.")
        $invalidTelemetry = $true
    }

    $sources = @($readyMarkers | ForEach-Object { $_.Groups["source"].Value }) +
        @($adc | ForEach-Object { $_.source })
    if ($sources.Count -gt 0) {
        $uniqueSources = @($sources | Sort-Object -Unique)
        if ($uniqueSources.Count -ne 1) {
            $sourceIssues.Add("Timing markers disagree on source: $($uniqueSources -join ', ').")
            $invalidTelemetry = $true
        } elseif (-not [string]::IsNullOrWhiteSpace($ExpectedSource) -and
            $uniqueSources[0] -ne $ExpectedSource) {
            $sourceIssues.Add("Timing source=$($uniqueSources[0]), expected $ExpectedSource.")
            $invalidTelemetry = $true
        }
    }

    foreach ($metric in $adc) {
        Test-KeypadTimingMetricShape $metric "ADC" $adcShapeIssues
    }
    foreach ($metric in $timer) {
        Test-KeypadTimingMetricShape $metric "ESP_TIMER_TASK" $timerShapeIssues
    }
    if ($adcShapeIssues.Count -gt 0) {
        $invalidTelemetry = $true
    }
    if ($timerShapeIssues.Count -gt 0) {
        $invalidTelemetry = $true
    }
    foreach ($issue in $adcShapeIssues) {
        $adcIssues.Add($issue)
    }
    foreach ($issue in $timerShapeIssues) {
        $timerIssues.Add($issue)
    }

    if ($adc.Count -lt $MinimumWindows) {
        $adcIssues.Add("Only $($adc.Count) ADC timing windows were observed; minimum is $MinimumWindows.")
    }
    if ($timer.Count -lt $MinimumWindows) {
        $timerIssues.Add("Only $($timer.Count) ESP_TIMER_TASK timing windows were observed; minimum is $MinimumWindows.")
    }

    $totalSnapshots = ($adc | Measure-Object -Property snapshots -Sum).Sum
    if ([uint64]$totalSnapshots -lt [uint64]$MinimumSnapshots) {
        $adcIssues.Add("Only $totalSnapshots successful ADC snapshots were observed; minimum is $MinimumSnapshots.")
    }

    $source = if ($sources.Count -gt 0) { $sources[0] } else { "UNKNOWN" }
    $sourcePass = $sourceIssues.Count -eq 0
    $adcPass = $sourcePass -and $adcIssues.Count -eq 0
    $timerPass = $sourcePass -and $timerIssues.Count -eq 0
    $allIssues = @($sourceIssues + $adcIssues + $timerIssues)

    return [pscustomobject]@{
        Pass = $sourcePass -and $adcPass -and $timerPass
        InvalidTelemetry = $invalidTelemetry
        SourcePass = $sourcePass
        AdcPass = $adcPass
        TimerPass = $timerPass
        Source = $source
        AdcMarkers = $adc
        TimerMarkers = $timer
        AdcDetail = if ($adcIssues.Count -eq 0) {
            Format-KeypadTimingAggregate $adc "ADC"
        } else {
            ($adcIssues -join "; ")
        }
        TimerDetail = if ($timerIssues.Count -eq 0) {
            Format-KeypadTimingAggregate $timer "ESP_TIMER_TASK"
        } else {
            ($timerIssues -join "; ")
        }
        SourceDetail = if ($sourceIssues.Count -eq 0) {
            "source=$source; timing profile marker is consistent."
        } else {
            ($sourceIssues -join "; ")
        }
        Detail = if ($allIssues.Count -eq 0) {
            "$(Format-KeypadTimingAggregate $adc 'ADC') $(Format-KeypadTimingAggregate $timer 'ESP_TIMER_TASK')"
        } else {
            ($allIssues -join "; ")
        }
    }
}
