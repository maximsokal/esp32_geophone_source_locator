# Local verification workflow

Normal project verification is local. GitHub Actions are not part of the routine
push/verification path. This file is the project-wide `Run All` instruction;
the detailed nRF54L15 matrix is in [`nrf54l15-test-task.md`](nrf54l15-test-task.md).

## Deterministic software verification

From the repository root:

```powershell
python tools/run_local_verify.py --scope software
```

This runs the native host manifests, complete non-HIL Python suite, and WebUI
install/typecheck/tests/build/bundle verification. It does not flash or monitor
hardware.

For the ESP32-S3 production firmware build:

```powershell
python tools/run_local_verify.py --scope firmware --jobs 2
```

Windows uses the repository's ESP-IDF 5.5.4 wrapper. No GitHub Actions fallback
is used.

## Canonical nRF54L15 production build

The nRF54L15 production toolchain is **nRF Connect SDK v3.4.0**. PlatformIO is
not the production authority after MAX-96.

From an ordinary Windows PowerShell:

```powershell
.\nRF54L15\ncs.ps1 build
```

The wrapper is pinned to NCS v3.4.0 and the upstream board target
`xiao_nrf54l15/nrf54l15/cpuapp`. If the current shell does not expose `west`,
it relaunches itself through Nordic sdk-manager using the exact pinned SDK. The
checkout may contain spaces: a temporary no-space `SUBST` source alias is used,
and generated output lives under:

```text
%USERPROFILE%\.ncs-workspaces\esp32_geophone_source_locator
```

The canonical production build directory is:

```text
nrf54l15-production-v3.4.0
```

`build` uses `nRF54L15/zephyr/prj.conf` only. The historical
`nRF54L15/ncs/channel_sounding.conf` remains the MAX-45 proof overlay until
MAX-51 promotes reviewed RAS/Channel-Sounding options into production.

To reproduce the proof:

```powershell
.\nRF54L15\ncs.ps1 cs-proof
.\nRF54L15\ncs.ps1 compare
.\nRF54L15\ncs.ps1 probe
```

These are not alternate production images.

## nRF local verification scope

Use:

```powershell
python tools/run_local_verify.py --scope nrf
```

This performs:

1. the native `coverage-nrf` host-policy suite;
2. the canonical NCS v3.4.0 production build;
3. the NCS-native embedded-test boundary.

The third stage currently reports `SKIPPED`: there is no reviewed NCS-native
`test_sw_*` runner yet. Hardware/HIL suites are intentionally excluded. A
PlatformIO `--without-uploading` run is not silently substituted, because
`--without-uploading` alone does not prove a test is hardware-free.

The separately qualified dependency-free native codec suite can be invoked with:

```powershell
.\nRF54L15\pio.ps1 test
```

This is compatibility/software evidence, not the canonical NCS embedded-test
stage; it reports `BLOCKED` rather than `SKIPPED` when host `gcc` is missing.

`python tools/run_local_verify.py --scope all --jobs 2` and VS Code
`Tests: All (no flash)` use the same NCS production build. They never flash nRF
hardware.

## Explicit nRF hardware actions

Canonical flash:

```powershell
.\nRF54L15\ncs.ps1 flash
```

This rebuilds/probes the production image and calls the resolved NCS/west board
runner. Flashing is explicit and never part of `Run All`.

Serial log monitor:

```powershell
.\nRF54L15\ncs.ps1 monitor -Port COM7
```

The monitor defaults to 115200 baud. Opening a serial stream is not a HIL pass.

VS Code exposes the same authority:

- `Build: nRF54L15 production (NCS)`;
- `Tests: nRF54L15`;
- `nRF54L15: Flash firmware`;
- `nRF54L15: Monitor logs`;
- `nRF54L15: PlatformIO native software tests`.

## PlatformIO compatibility boundary

`nRF54L15/platformio.ini` and `pio.ps1` remain for compatibility smoke against
the previously verified Seeed PlatformIO/Zephyr 4.2.1 stack. The corresponding
VS Code task is explicitly named:

```text
nRF54L15: PlatformIO compatibility build
```

That artifact is not the official firmware, must not be used as Channel
Sounding controller evidence, and is not used by `--scope nrf` or `Run All`.
The production flash task never calls `pio.ps1 upload`.

The compatibility build selects `nRF54L15/zephyr/prj.platformio.conf`, which
contains only the shared GPIO/I2C/runtime settings. The NCS-only
SoftDevice/RAS/Channel Sounding symbols remain in `zephyr/prj.conf` and are
not silently weakened for the canonical production build.

## nRF hardware contract relevant to verification

The installed target is the plain XIAO nRF54L15, not the Sense variant. Remote
nodes use an external GY-BMI160 and BME280 on the XIAO private second-I2C pair
(D11/P0.03 SCL, D12/P0.04 SDA), powered from XIAO `3V3_OUT`. The main-enclosure
node does not require the remote-only sensor assembly. Upstream D4/P1.10 SDA +
D5/P1.11 SCL use `i2c22` as TWIS toward ESP32/TCA9548A. RJ45 pin 2 (+3.3 V) is
RESERVED/NC.

The common firmware and wire contracts remain:

- one runtime MAIN/REMOTE image;
- 12-byte `SENSOR_REFRESH`;
- 112-byte REMOTE sensor snapshot;
- no geophone TDOA/cross-node clock assumptions from nRF uptime fields.

A successful build proves none of the physical wiring, sensor behavior or radio
accuracy.

## Master project-wide validation

Hardware-free master entry points:

```powershell
python tools/run_local_verify.py --scope all --jobs 2
```

or VS Code `Tests: All (no flash)` / `tools/run_suite.ps1 -Suite All`.

The master contract is:

1. ESP32 host/Python/WebUI deterministic surfaces;
2. nRF host-policy + canonical NCS production build;
3. qualified software/native nRF tests only when an NCS-native runner is
   explicitly reviewed;
4. ESP32 Unity/production builds;
5. no implicit hardware access.

Physical tiers stay separate:

| Tier | Evidence | Physical prerequisite |
| --- | --- | --- |
| Software-only | host/Python/WebUI, ESP32 builds, nRF host policy + NCS build | none |
| ESP32 target | Unity/production live telemetry | physical ESP32-S3 |
| One nRF54L15 | boot/logs/upstream I2C; remote profile adds BMI160/BME280/LEDs | one XIAO |
| Multiple nRF54L15 | pairwise CS, reconnect, role rotation | two or more XIAOs |
| Full topology | ESP32 + TCA + main/remote XIAOs and remote sensors | complete wiring |

Missing physical hardware is `BLOCKED`, never an implicit pass.

## Narrow scopes

```powershell
python tools/run_local_verify.py --scope host
python tools/run_local_verify.py --scope python
python tools/run_local_verify.py --scope web
python tools/run_local_verify.py --scope nrf
```

`--dry-run` prints the planned commands without executing them.

## Evidence boundary

Software/build evidence does not prove physical keypad/TFT behavior, SPI/SD
latency, DMA timing, GPIOs, actual GY-BMI160/BME280 wiring, Bluetooth Channel
Sounding accuracy, RF environment, or complete TCA topology. Those remain
explicit live-board/HIL evidence as defined in
[`verification-evidence-matrix.md`](verification-evidence-matrix.md).
