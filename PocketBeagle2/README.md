# PocketBeagle 2 Rev A0 Subproject (`PocketBeagle2/`)

Independent C++17 / CMake / Ninja / CTest subproject for **BeagleBoard PocketBeagle 2 Revision A0** (TI AM6232, dual Cortex-A53, 512 MB RAM, PRU-SS), isolated from the root ESP-IDF (`esp32s3`) and `nRF54L15/` (Zephyr/PlatformIO) build graphs.

## Quick Commands (WSL2 / Linux)
```bash
cd PocketBeagle2
cmake --preset host-debug && cmake --build --preset host-debug && ctest --preset host-debug
cmake --preset arm64-release && cmake --build --preset arm64-release
```

## Quick Commands (Windows PowerShell)
```powershell
.\PocketBeagle2\scripts\pb2.ps1 -Action test-host
.\PocketBeagle2\scripts\pb2.ps1 -Action build-arm64
.\PocketBeagle2\scripts\pb2.ps1 -Action doctor
.\PocketBeagle2\scripts\pb2.ps1 -Action deploy
```

## Hardware Profiles
See `config/board_a0.json` for the single source of truth on:
- Debian 13.3 defaults (`192.168.7.2`, user `debian`)
- UART2 (`P2.04 TX`, `P2.02 RX`, `P2.15 GND`) and unverified ESP32-S3 DevKitC v1.0/v1.1 profiles
- AD7606 4-geophone profiles (`SERIAL_SINGLE_DOUT` and priority `SERIAL_DUAL_DOUT`)
