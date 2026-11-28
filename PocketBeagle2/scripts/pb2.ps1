param(
    [Parameter(Mandatory = $true)]
    [ValidateSet(
        "doctor",
        "build-host",
        "test-host",
        "test-board",
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
        "flash-sd",
        "test-virtual-pru",
        "test-virtual-pipeline",
        "test-virtual-uart",
        "test-virtual-dual-board",
        "test-all-virtual"
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
    "test-board" {
        Assert-BoardReachable
        $protoDir = Join-Path $repoRootWin "components\pb2_protocol"
        $eventsDir = Join-Path $repoRootWin "components\geophone_events"
        $locDir = Join-Path $repoRootWin "components\localization"
        $pb2Src = Join-Path $pb2RootWin "src"
        $pb2Inc = Join-Path $pb2RootWin "include"
        $pb2PruInc = Join-Path $pb2RootWin "pru\include"
        $pb2Tests = Join-Path $pb2RootWin "tests"

        & ssh "$Pb2User@$Pb2Host" "mkdir -p '$RemoteDir/src_build/proto' '$RemoteDir/src_build/events' '$RemoteDir/src_build/loc/stubs' '$RemoteDir/src_build/pb2'"
        & scp -r "$protoDir\*" "${Pb2User}@${Pb2Host}:$RemoteDir/src_build/proto/"
        & scp -r "$eventsDir\*.c" "$eventsDir\include" "${Pb2User}@${Pb2Host}:$RemoteDir/src_build/events/"
        & scp "$locDir\localization.c" "${Pb2User}@${Pb2Host}:$RemoteDir/src_build/loc/"
        & scp -r "$locDir\include" "${Pb2User}@${Pb2Host}:$RemoteDir/src_build/loc/"
        & scp "$locDir\tests\stubs\esp_err.h" "${Pb2User}@${Pb2Host}:$RemoteDir/src_build/loc/stubs/"
        & scp -r "$pb2Inc" "$pb2Src" "$pb2Tests" "${Pb2User}@${Pb2Host}:$RemoteDir/src_build/pb2/"
        & scp -r "$pb2PruInc" "${Pb2User}@${Pb2Host}:$RemoteDir/src_build/pb2/pru_include"

        $buildAndRun = @'
set -e
cd ~/pb2_locator/src_build
mkdir -p bin
gcc -std=c11 -O2 -Wall -Wextra -Werror -Iproto/include -c proto/pb2_cobs.c -o bin/pb2_cobs.o
gcc -std=c11 -O2 -Wall -Wextra -Werror -Iproto/include -c proto/pb2_crc32c.c -o bin/pb2_crc32c.o
gcc -std=c11 -O2 -Wall -Wextra -Werror -Iproto/include -c proto/pb2_protocol.c -o bin/pb2_protocol.o
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ipb2/include -Ipb2/pru_include -Iproto/include \
  pb2/src/acquisition/mock_source.cpp pb2/src/acquisition/pru_ring_consumer.cpp \
  pb2/src/diagnostics/runtime_info.cpp pb2/src/hardware/pinmap_a0.cpp \
  pb2/src/power/shutdown_fsm.cpp pb2/src/storage/geoseis_v2_writer.cpp \
  pb2/src/transport/uart_transport.cpp bin/pb2_cobs.o bin/pb2_crc32c.o bin/pb2_protocol.o \
  -c 2>/dev/null || true
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ipb2/include -Ipb2/pru_include -Iproto/include pb2/src/acquisition/mock_source.cpp -c -o bin/mock_source.o
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ipb2/include -Ipb2/pru_include -Iproto/include pb2/src/acquisition/pru_ring_consumer.cpp -c -o bin/pru_ring_consumer.o
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ipb2/include -Ipb2/pru_include -Iproto/include pb2/src/diagnostics/runtime_info.cpp -c -o bin/runtime_info.o
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ipb2/include -Ipb2/pru_include -Iproto/include pb2/src/hardware/pinmap_a0.cpp -c -o bin/pinmap_a0.o
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ipb2/include -Ipb2/pru_include -Iproto/include pb2/src/power/shutdown_fsm.cpp -c -o bin/shutdown_fsm.o
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ipb2/include -Ipb2/pru_include -Iproto/include pb2/src/storage/geoseis_v2_writer.cpp -c -o bin/geoseis_v2_writer.o
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ipb2/include -Ipb2/pru_include -Iproto/include pb2/src/transport/uart_transport.cpp -c -o bin/uart_transport.o
CORE_OBJS="bin/mock_source.o bin/pru_ring_consumer.o bin/runtime_info.o bin/pinmap_a0.o bin/shutdown_fsm.o bin/geoseis_v2_writer.o bin/uart_transport.o bin/pb2_cobs.o bin/pb2_crc32c.o bin/pb2_protocol.o"
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ipb2/include -Ipb2/pru_include -Iproto/include pb2/tests/unit/test_mock_source.cpp $CORE_OBJS -o bin/pb2_unit_tests
gcc -std=c11 -O2 -Wall -Wextra -Werror -Iproto/include proto/tests/pb2_protocol_test.c bin/pb2_cobs.o bin/pb2_crc32c.o bin/pb2_protocol.o -lm -o bin/pb2_protocol_c_tests
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ipb2/include -Ipb2/pru_include -Iproto/include pb2/tests/integration/test_pty_transport.cpp $CORE_OBJS -o bin/pb2_pty_tests
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ipb2/include -Ipb2/pru_include -Iproto/include pb2/tests/unit/test_phase3_4.cpp $CORE_OBJS -o bin/pb2_phase3_4_tests
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ipb2/include -Ipb2/pru_include -Iproto/include pb2/tests/unit/test_geoseis_v2.cpp $CORE_OBJS -o bin/pb2_geoseis_v2_tests
gcc -std=c11 -O2 -Ievents/include -Iloc/include -Iloc/stubs -c events/*.c loc/localization.c
mv *.o bin/
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Ievents/include -Iloc/include -Iloc/stubs pb2/tests/unit/test_shared_math.cpp bin/geophone_event_*.o bin/localization.o -lm -o bin/pb2_shared_math_tests
for t in pb2_unit_tests pb2_protocol_c_tests pb2_pty_tests pb2_phase3_4_tests pb2_geoseis_v2_tests pb2_shared_math_tests; do
  echo "=== $t (on-board aarch64) ==="
  ./bin/$t
done
'@
        $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($buildAndRun))
        & ssh "$Pb2User@$Pb2Host" "echo '$b64' | base64 -d | bash"
        exit $LASTEXITCODE
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
    "build-pru" {
        Assert-BoardReachable
        $pruDir = Join-Path $pb2RootWin "pru"
        & ssh "$Pb2User@$Pb2Host" "mkdir -p '$RemoteDir/pru'"
        & scp -r "$pruDir\*" "${Pb2User}@${Pb2Host}:$RemoteDir/pru/"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & ssh "$Pb2User@$Pb2Host" "cd '$RemoteDir/pru' && make clean all"
        exit $LASTEXITCODE
    }
    "load-pru" {
        Assert-BoardReachable
        $remoteScript = @'
set -e
RP=""
for d in /sys/class/remoteproc/remoteproc*; do
  if [ "$(cat "$d/name" 2>/dev/null)" = "30078000.pru" ]; then
    RP="$d"
    break
  fi
done
[ -n "$RP" ] || { echo "30078000.pru not found"; exit 1; }
echo "Discovered PRU1 at $RP"
sudo cp /home/debian/pb2_locator/pru/build/pb2_pru1_ad7606.out /lib/firmware/pb2_pru1_ad7606.out
STATE="$(cat "$RP/state")"
if [ "$STATE" = "running" ]; then
  echo stop | sudo tee "$RP/state" >/dev/null
fi
echo pb2_pru1_ad7606.out | sudo tee "$RP/firmware" >/dev/null
echo start | sudo tee "$RP/state" >/dev/null
echo "PRU1 state: $(cat "$RP/state") firmware: $(cat "$RP/firmware")"
'@
        $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($remoteScript))
        & ssh -t "$Pb2User@$Pb2Host" "echo '$b64' | base64 -d | bash"
        exit $LASTEXITCODE
    }
    "stop-pru" {
        Assert-BoardReachable
        $remoteScript = @'
set -e
for d in /sys/class/remoteproc/remoteproc*; do
  if [ "$(cat "$d/name" 2>/dev/null)" = "30078000.pru" ]; then
    if [ "$(cat "$d/state")" = "running" ]; then
      echo stop | sudo tee "$d/state" >/dev/null
    fi
    echo "PRU1 state: $(cat "$d/state")"
  fi
done
'@
        $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($remoteScript))
        & ssh -t "$Pb2User@$Pb2Host" "echo '$b64' | base64 -d | bash"
        exit $LASTEXITCODE
    }
    { $_ -in @("install-overlay", "hil-uart", "hil-adc", "benchmark-adc", "benchmark-storage", "benchmark-transport") } {
        Write-Host "[pb2] Action '$Action' is reserved for Phase 2-5 (gated by plan approval and hardware)." -ForegroundColor Yellow
        exit 0
    }
    "test-virtual-pru" {
        $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
        $cgt = Join-Path $repoRoot ".tools\ti-cgt-pru-2.3.3\ti-cgt-pru_2.3.3"
        $clpru = Join-Path $cgt "bin\clpru.exe"
        if (-not (Test-Path $clpru)) {
            throw "TI PRU CGT not found at $clpru"
        }
        Push-Location $repoRoot
        try {
            & $clpru -v3 -O2 --endian=little -I"$cgt/include" -IPocketBeagle2/pru/include --define=PB2_PRU_HOST_ADAPTER=0 PocketBeagle2/pru/acquisition/main_pru1.c PocketBeagle2/pru/acquisition/pru1_ad7606.c -z -i"$cgt/lib" "-llibc.a" PocketBeagle2/pru/linker/AM6232_PRU1.cmd -o PocketBeagle2/pru/build/pb2_pru1_ad7606.out
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
            python -m pytest tests/python/test_pru_ad7606_sim.py -v
            exit $LASTEXITCODE
        } finally {
            Pop-Location
        }
    }
    "test-virtual-pipeline" {
        Invoke-WslBash "cmake -S . -B build_host -DPB2_BUILD_TESTS=ON && cmake --build build_host -j && ctest --test-dir build_host -R 'pb2_phase3_4_tests|pb2_pipeline_tests|pb2_geoseis_v2_tests|pb2_unit_tests|pb2_shared_math_tests' --output-on-failure"
    }
    "test-virtual-uart" {
        Invoke-WslBash "cmake -S . -B build_host -DPB2_BUILD_TESTS=ON && cmake --build build_host -j && ctest --test-dir build_host -R 'pb2_pty_tests|pb2_protocol_c_tests' --output-on-failure"
    }
    "test-virtual-dual-board" {
        $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
        Push-Location $repoRoot
        try {
            python -m pytest tests/python/test_virtual_dual_board.py -v
            exit $LASTEXITCODE
        } finally {
            Pop-Location
        }
    }
    "test-all-virtual" {
        $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
        Push-Location $repoRoot
        try {
            Write-Host "=== 1/5 ESP32 Virtual Board Stress ===" -ForegroundColor Cyan
            powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repoRoot "tools\run_virtual_board_stress.ps1")
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

            Write-Host "=== 2/5 PRU1 ELF Build & TI Simulator Tests ===" -ForegroundColor Cyan
            powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Action test-virtual-pru
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

            Write-Host "=== 3/5 PocketBeagle 2 Host CTest (7 suites) ===" -ForegroundColor Cyan
            Invoke-WslBash "cmake -S . -B build_host -DPB2_BUILD_TESTS=ON && cmake --build build_host -j && ctest --test-dir build_host --output-on-failure"
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

            Write-Host "=== 4/5 Virtual Dual-Board E2E (PRU -> GEOSEIS1 v2, PTY, Shared Math) ===" -ForegroundColor Cyan
            python -m pytest tests/python/test_virtual_dual_board.py -v
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

            Write-Host "=== 5/5 ast-grep Verification ===" -ForegroundColor Cyan
            python tools/run_local_verify.py --scope ast
            exit $LASTEXITCODE
        } finally {
            Pop-Location
        }
    }
}
