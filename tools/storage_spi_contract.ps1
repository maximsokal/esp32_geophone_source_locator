Set-StrictMode -Version Latest


function Get-StorageSpiTelemetry {
    param([string]$Content)

    $begin = @()
    foreach ($match in [regex]::Matches(
        $Content,
        '(?im)STORAGE_SPI_TIMING\s+stage=begin\s+mutex_wait_us=(?<mutex>\d+)\s+tft_quiesce_us=(?<quiesce>\d+)\s+result=(?<result>-?\d+)'
    )) {
        $begin += [pscustomobject]@{
            MutexWaitUs = [uint64]$match.Groups['mutex'].Value
            TftQuiesceUs = [uint64]$match.Groups['quiesce'].Value
            Result = [int]$match.Groups['result'].Value
        }
    }

    $end = @()
    foreach ($match in [regex]::Matches(
        $Content,
        '(?im)STORAGE_SPI_TIMING\s+stage=end\s+tft_resume_us=(?<resume>\d+)\s+elapsed_us=(?<elapsed>\d+)'
    )) {
        $end += [pscustomobject]@{
            TftResumeUs = [uint64]$match.Groups['resume'].Value
            ElapsedUs = [uint64]$match.Groups['elapsed'].Value
        }
    }

    $recorder = @()
    foreach ($match in [regex]::Matches(
        $Content,
        '(?im)RECORDER_IO_TIMING\s+op=fwrite\s+requested=(?<requested>\d+)\s+written=(?<written>\d+)\s+windows=(?<windows>\d+)\s+max_window_us=(?<window>\d+)\s+fwrite_us=(?<fwrite>\d+)\s+operation_us=(?<operation>\d+)\s+result=(?<result>-?\d+)'
    )) {
        $recorder += [pscustomobject]@{
            Requested = [uint64]$match.Groups['requested'].Value
            Written = [uint64]$match.Groups['written'].Value
            Windows = [uint64]$match.Groups['windows'].Value
            MaxWindowUs = [uint64]$match.Groups['window'].Value
            FwriteUs = [uint64]$match.Groups['fwrite'].Value
            OperationUs = [uint64]$match.Groups['operation'].Value
            Result = [int]$match.Groups['result'].Value
        }
    }

    return [pscustomobject]@{
        StorageBegin = $begin
        StorageEnd = $end
        RecorderFwrite = $recorder
    }
}


function Get-MaxTelemetryValue {
    param(
        [object[]]$Items,
        [string]$Property
    )

    if ($null -eq $Items -or $Items.Count -eq 0) {
        return $null
    }

    $maximum = [uint64]0
    foreach ($item in $Items) {
        $value = [uint64]$item.$Property
        if ($value -gt $maximum) {
            $maximum = $value
        }
    }
    return $maximum
}


function Test-StorageSpiTelemetry {
    param(
        [string]$Content,
        [uint64]$MaxMutexWaitUs,
        [uint64]$MaxTftQuiesceUs,
        [uint64]$MaxTftResumeUs,
        [uint64]$MaxSpiEndElapsedUs,
        [uint64]$MaxRecorderWindowUs,
        [uint64]$MaxRecorderFwriteUs,
        [uint64]$MaxRecorderOperationUs,
        [int]$MinimumStorageSamples = 1,
        [int]$MinimumRecorderSamples = 1
    )

    $telemetry = Get-StorageSpiTelemetry $Content
    $begin = @($telemetry.StorageBegin)
    $end = @($telemetry.StorageEnd)
    $recorder = @($telemetry.RecorderFwrite)

    $missing = @()
    if ($begin.Count -lt $MinimumStorageSamples) {
        $missing += "STORAGE_SPI_TIMING stage=begin ($($begin.Count)/$MinimumStorageSamples)"
    }
    if ($end.Count -lt $MinimumStorageSamples) {
        $missing += "STORAGE_SPI_TIMING stage=end ($($end.Count)/$MinimumStorageSamples)"
    }
    if ($recorder.Count -lt $MinimumRecorderSamples) {
        $missing += "RECORDER_IO_TIMING op=fwrite ($($recorder.Count)/$MinimumRecorderSamples)"
    }

    $violations = @()

    foreach ($sample in $begin) {
        if ($sample.Result -ne 0) {
            $violations += "storage begin result=$($sample.Result)"
        }
    }

    foreach ($sample in $recorder) {
        if ($sample.Result -ne 0) {
            $violations += "recorder fwrite result=$($sample.Result)"
        }
        if ($sample.Written -ne $sample.Requested) {
            $violations += "recorder short write requested=$($sample.Requested) written=$($sample.Written)"
        }
        if ($sample.Windows -eq 0) {
            $violations += "recorder fwrite reported zero storage windows"
        }
    }

    $maxMutex = Get-MaxTelemetryValue $begin 'MutexWaitUs'
    $maxQuiesce = Get-MaxTelemetryValue $begin 'TftQuiesceUs'
    $maxResume = Get-MaxTelemetryValue $end 'TftResumeUs'
    $maxEndElapsed = Get-MaxTelemetryValue $end 'ElapsedUs'
    $maxWindow = Get-MaxTelemetryValue $recorder 'MaxWindowUs'
    $maxFwrite = Get-MaxTelemetryValue $recorder 'FwriteUs'
    $maxOperation = Get-MaxTelemetryValue $recorder 'OperationUs'

    $checks = @(
        @{ Name = 'mutex_wait_us'; Value = $maxMutex; Limit = $MaxMutexWaitUs },
        @{ Name = 'tft_quiesce_us'; Value = $maxQuiesce; Limit = $MaxTftQuiesceUs },
        @{ Name = 'tft_resume_us'; Value = $maxResume; Limit = $MaxTftResumeUs },
        @{ Name = 'spi_end_elapsed_us'; Value = $maxEndElapsed; Limit = $MaxSpiEndElapsedUs },
        @{ Name = 'max_window_us'; Value = $maxWindow; Limit = $MaxRecorderWindowUs },
        @{ Name = 'fwrite_us'; Value = $maxFwrite; Limit = $MaxRecorderFwriteUs },
        @{ Name = 'recorder_operation_us'; Value = $maxOperation; Limit = $MaxRecorderOperationUs }
    )

    foreach ($check in $checks) {
        if ($null -ne $check.Value -and [uint64]$check.Value -gt [uint64]$check.Limit) {
            $violations += "$($check.Name)=$($check.Value)us exceeds limit=$($check.Limit)us"
        }
    }

    return [pscustomobject]@{
        Missing = $missing
        Violations = $violations
        StorageBeginCount = $begin.Count
        StorageEndCount = $end.Count
        RecorderCount = $recorder.Count
        MaxMutexWaitUs = $maxMutex
        MaxTftQuiesceUs = $maxQuiesce
        MaxTftResumeUs = $maxResume
        MaxSpiEndElapsedUs = $maxEndElapsed
        MaxRecorderWindowUs = $maxWindow
        MaxRecorderFwriteUs = $maxFwrite
        MaxRecorderOperationUs = $maxOperation
    }
}
