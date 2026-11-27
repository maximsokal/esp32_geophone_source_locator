param(
    [Parameter(Mandatory = $true)]
    [ValidateSet(
        "doctor",
        "build-host",
        "test-host",
        "build-arm64",
        "deploy",
        "run",
        "stop",
        "monitor",
        "debug",
        "build-pru",
        "load-pru",
        "stop-pru",
        "install-overlay",
        "hil-uart",
        "hil-adc",
        "benchmark-adc",
        "benchmark-storage",
        "benchmark-transport",
        "pull-logs",
        "clean",
        "flash-sd"
    )]
    [string]$Action,

    [int]$DiskNumber = -1,

    [string]$Pb2Host = $(if ($env:PB2_HOST) { $env:PB2_HOST } else { "192.168.7.2" }),
    [string]$Pb2User = $(if ($env:PB2_USER) { $env:PB2_USER } else { "debian" }),
    [string]$RemoteDir = $(if ($env:PB2_REMOTE_DIR) { $env:PB2_REMOTE_DIR } else { "/home/debian/pb2_locator" }),
    [string]$WslDistro = $(if ($env:PB2_WSL_DISTRO) { $env:PB2_WSL_DISTRO } else { "Ubuntu" }),
    [string]$Preset = "host-debug"
)

$ErrorActionPreference = "Stop"

$pb2RootWin = Split-Path -Parent $PSScriptRoot
$repoRootWin = Split-Path -Parent $pb2RootWin

function Convert-ToWslPath([string]$WinPath) {
    $resolved = (Resolve-Path $WinPath).Path
    if ($resolved -match '^([A-Za-z]):\\(.*)$') {
        $drive = $matches[1].ToLowerInvariant()
        $rest = $matches[2] -replace '\\', '/'
        return "/mnt/$drive/$rest"
    }
    throw "Cannot convert path to WSL path: $WinPath"
}

function Invoke-WslBash([string]$Script) {
    $pb2Wsl = Convert-ToWslPath $pb2RootWin
    $cmd = "cd '$pb2Wsl' && $Script"
    & wsl -d $WslDistro -- bash -lc $cmd
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Assert-BoardReachable {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & ssh -o BatchMode=yes -o ConnectTimeout=5 "$Pb2User@$Pb2Host" "true" 2>$null
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($rc -ne 0) {
        Write-Host "[pb2] SKIPPED / UNREACHABLE: Cannot reach $Pb2User@$Pb2Host via SSH." -ForegroundColor Yellow
        Write-Host "[pb2] Set PB2_HOST / PB2_USER or connect PocketBeagle 2 A0 over USB-C (192.168.7.2)." -ForegroundColor Yellow
        exit 2
    }
}

switch ($Action) {
    "build-host" {
        Invoke-WslBash "cmake --preset host-debug && cmake --build --preset host-debug"
    }
    "test-host" {
        Invoke-WslBash "cmake --preset host-debug && cmake --build --preset host-debug && ctest --preset host-debug"
    }
    "build-arm64" {
        Invoke-WslBash "cmake --preset arm64-release && cmake --build --preset arm64-release"
    }
    "clean" {
        $buildDir = Join-Path $pb2RootWin "build"
        if (Test-Path $buildDir) {
            Remove-Item -Recurse -Force $buildDir
            Write-Host "[pb2] Cleaned $buildDir"
        } else {
            Write-Host "[pb2] Nothing to clean ($buildDir absent)"
        }
    }
    "flash-sd" {
        if ($DiskNumber -lt 0) {
            Get-Disk | Where-Object { $_.BusType -eq "USB" -or $_.BusType -eq "SD" } | Format-Table Number,FriendlyName,BusType,@{N='SizeGB';E={[math]::Round($_.Size/1GB,2)}}
            throw "Specify -DiskNumber <N> (only USB/SD removable disks are allowed)."
        }
        $flashScript = Join-Path $PSScriptRoot "deploy\flash_sd.ps1"
        & $flashScript -DiskNumber $DiskNumber
        exit $LASTEXITCODE
    }
    "doctor" {
        Assert-BoardReachable
        $docPath = Join-Path $PSScriptRoot "doctor\pb2_doctor.sh"
        & scp $docPath "${Pb2User}@${Pb2Host}:/tmp/pb2_doctor.sh"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & ssh "$Pb2User@$Pb2Host" "sed -i 's/\r$//' /tmp/pb2_doctor.sh && bash /tmp/pb2_doctor.sh"
        exit $LASTEXITCODE
    }
    "deploy" {
        Invoke-WslBash "cmake --preset arm64-release && cmake --build --preset arm64-release"
        Assert-BoardReachable
        $bin = Join-Path $pb2RootWin "build\arm64-release\pb2_node"
        $cfg = Join-Path $pb2RootWin "config\board_a0.json"
        & ssh "$Pb2User@$Pb2Host" "mkdir -p '$RemoteDir/bin' '$RemoteDir/config'"
        & scp $bin "${Pb2User}@${Pb2Host}:$RemoteDir/bin/pb2_node"
        & scp $cfg "${Pb2User}@${Pb2Host}:$RemoteDir/config/board_a0.json"
        & ssh "$Pb2User@$Pb2Host" "chmod +x '$RemoteDir/bin/pb2_node'"
        Write-Host "[pb2] Deployed pb2_node to ${Pb2User}@${Pb2Host}:$RemoteDir"
    }
    "run" {
        Assert-BoardReachable
        & ssh "$Pb2User@$Pb2Host" "'$RemoteDir/bin/pb2_node' --doctor-info --mock-smoke"
        exit $LASTEXITCODE
    }
    "stop" {
        Assert-BoardReachable
        & ssh "$Pb2User@$Pb2Host" "pkill -TERM -x pb2_node || true"
    }
    "monitor" {
        Assert-BoardReachable
        & ssh "$Pb2User@$Pb2Host" "journalctl -n 50 --no-pager"
    }
    "debug" {
        Invoke-WslBash "cmake --preset arm64-debug && cmake --build --preset arm64-debug"
        Assert-BoardReachable
        $bin = Join-Path $pb2RootWin "build\arm64-debug\pb2_node"
        & ssh "$Pb2User@$Pb2Host" "mkdir -p '$RemoteDir/bin'"
        & scp $bin "${Pb2User}@${Pb2Host}:$RemoteDir/bin/pb2_node"
        Write-Host "[pb2] Starting gdbserver on ${Pb2Host}:2345 (attach from WSL2 with gdb-multiarch)..."
        & ssh "$Pb2User@$Pb2Host" "gdbserver :2345 '$RemoteDir/bin/pb2_node' --doctor-info --mock-smoke"
    }
    "pull-logs" {
        Assert-BoardReachable
        $logOut = Join-Path $pb2RootWin "logs"
        New-Item -ItemType Directory -Force $logOut | Out-Null
        $ts = Get-Date -Format "yyyyMMdd-HHmmss"
        $dest = Join-Path $logOut "pb2_journal_$ts.log"
        & ssh "$Pb2User@$Pb2Host" "journalctl -b --no-pager" | Out-File -Encoding utf8 $dest
        Write-Host "[pb2] Saved $dest"
    }
    { $_ -in @("build-pru", "load-pru", "stop-pru", "install-overlay", "hil-uart", "hil-adc", "benchmark-adc", "benchmark-storage", "benchmark-transport") } {
        Write-Host "[pb2] Action '$Action' is reserved for Phase 2-5 (gated by plan approval and hardware)." -ForegroundColor Yellow
        exit 0
    }
}
