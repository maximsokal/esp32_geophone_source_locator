# Firmware test plan

Status: current as of 2026-09-14. Older test counts, local-machine snapshots and migration notes are preserved in Git history; they are not current acceptance evidence.

The authoritative distinction between host tests, source contracts, replay/parser tests, live-board HIL and manual physical observations is in [`verification-evidence-matrix.md`](verification-evidence-matrix.md). No green replay or source-contract check may be reported as physical hardware proof.

## Production baseline

- Target: ESP32-S3, ESP-IDF 5.5.4.
- Production firmware is built by `.github/workflows/device-console-verify.yml` in the official `espressif/idf:v5.5.4` container.
- Native host tests are manifest-driven from `tests/host/manifest.json`; the manifest is the source of truth for suite count.
- Web Console, XR and typecheck run independently from firmware host tests.
- COM4 is the programming port. The passive UART adapter is a separate port, normally COM6; its TX/VCC pins are not connected to the target.
- `nRF54L15/` is an independent PlatformIO + Zephyr project. It has one
  production `seeed-xiao-nrf54l15` environment for every installed node plus a
  separate native environment for qualified software tests. It is built through
  `nRF54L15/pio.ps1`, not through the ESP-IDF root build.
- Installed nRF hardware is the plain XIAO nRF54L15, not the Sense variant.
  Remote geophone nodes use an external GY-BMI160 and BME280 on a private local
  I2C bus (`D11/P0.03` SCL, `D12/P0.04` SDA), powered from XIAO `3V3_OUT` and
  aggregated by the remote nRF; the main-enclosure nRF does not require that
  remote-only sensor assembly.

## Automated test layers

### 1. Native C host tests

Run:

```text
python tools/run_host_tests.py --keep-going
```

These compile selected production C modules against deterministic ESP-IDF/FreeRTOS/component fakes. They are intended for state machines, lifecycle/error handling, queue/lock ownership, policy and pure data-path behavior. They do not prove ESP32 timing, dual-core behavior, DMA, real peripheral electrical behavior or physical UI output.

Critical covered areas include APP_EVENT delivery, ADC service settling, runtime observability and allocation policy, SD mount/diagnostics, AD7606 policy/driver/serial behavior, keypad decode/ADC ownership/timing helpers, localization, seismic DSP, measurement service, object format/store/recovery, SPI arbitration, power-button policy, seismic buffer/recorder, storage manager, Web server policy/routes and UI trace scheduling. Coverage debt is tracked in Linear rather than inferred from file names.

### 2. Python source/architecture contracts

Run the deterministic Python suite or the explicit files used by CI. These checks lock source/configuration invariants such as startup ordering, responsive object-store usage and runtime-priority configuration. They prove architecture/configuration constraints only; they do not execute the target runtime.

### 2a. Software-only virtual-board stress model

`tools/run_virtual_board_stress.ps1` selects the `virtual_board_stress` native
host case. The case executes the production-compatible `seismic_buffer` and
recorder-cache code with a discrete virtual clock and a virtual board model.
The model covers AD7606 conversion/BUSY/SPI3 at 1 MHz, shared SD/TFT SPI2 at
20 MHz, Wi-Fi events, INA3221/TCA9548A I2C transactions, bounded queues, TFT
redraws, recorder write-behind backpressure, deterministic SD jitter and a
200 ms storage stall. Its JSON lines report produced/recorded/dropped frames,
cache and queue high-water marks, protocol-model counters, deadline misses and
SD p99 latency.

The runner prints `VIRTUAL_BOARD_PHYSICAL_IO: NONE`, never opens a COM port,
`VIRTUAL_BOARD_TARGET_REQUIRED: NO`, never flashes firmware and never powers a
board. `tools/run_stress_tests.ps1`
remains a compatibility alias for the same software-only suite.

This follows the host-testing direction documented by ESP-IDF: use the
POSIX/Linux FreeRTOS simulator for host scheduling or CMock/mock components
for hardware-dependent code. The Windows implementation here uses a
deterministic native mock model, not a physical ESP32 target. The target
integration stress application below is a separate optional gate and is never
part of the virtual bottleneck-search command. See the official
[ESP-IDF host-application guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/host-apps.html).

The suite includes healthy scenarios and an overloaded case. A healthy
scenario requires no drops, cache overflow, queue overflow or deadline miss;
the overloaded scenario must be classified as `SATURATED`. This is the
primary bottleneck-detection loop and is deliberately safe to run repeatedly
on the PC. It is not physical-board evidence: exact ESP32 cache/DMA behavior,
SD media tails, signal integrity, power rails and TFT pixels remain separate
target/HIL concerns.

### 2b. Optional ESP32 target integration stress

`tests/target/stress_app` is an isolated ESP-IDF 5.5.4 project and does not
modify the production `main` application. It links the production seismic ring
buffer and recorder cache and creates actual pinned FreeRTOS tasks on both
cores. The producer runs synthetic rates above the current 50 Hz test generator;
the recorder models SD service delay; UI, sensor and network tasks create
contention; internal DMA heap and PSRAM allocations are exercised; task stack
high-water marks, heap snapshots, ISR-dispatch ticks and a bounded event trace
are printed; a deterministic 64-event reordered schedule is replayed and
validated by seed/hash/inversion markers. The application runs 100..5000 Hz
rate-sweep phases, with 5000 Hz expected as the saturation control point; it
also binary-searches the first failed rate and runs an intentional 5000 Hz/
slow-SD saturation control.

Build evidence is target compilation only until the image is run on an ESP32-S3:

```powershell
powershell -File tools/run_idf.ps1 -Action build `
  -ProjectPath tests/target/stress_app -BuildDir build-s3 -Target esp32s3 -Jobs 2
```

`tools/run_stress_hil.ps1` owns optional target stress replay/live execution. Its saved
log mode performs no hardware operation. The intended two-USB topology is
COM4 for programming and board power, with COM6 receive-only for UART logs;
keep COM6 TX/VCC disconnected. The same USB cable is used first for COM4
flashing from the PC, then physically unplugged from the PC and connected to the
USB 18650 block. Its live mode can build, flash via COM4 and capture passive UART
via COM6; `-WaitForPowerHandoff` makes the physical sequence explicit: flash first,
move that same cable to the USB 18650 block, open the COM6 monitor, then
press EN/RESET for the captured run. `-SkipBuild` reuses a previously verified
target image while preserving the flash/log steps. Live `-RequirePhysical` also
requires `-WaitForPowerHandoff`, preventing a COM4-powered run from being accepted
as AD7606 evidence. Before synthetic stress, the image runs production-driver probes and
prints `TARGET_STRESS_PHYSICAL` markers for repeated AD7606 conversion/payload,
SD write-read, TFT draw, I2C INA/TCA access and Wi-Fi radio scan. The runner
requires those probe markers, plus the explicit operator confirmations, when
`-RequirePhysical` is used. TFT pixels and Wi-Fi association/API remain manual
observations. It prints `STRESS_RUN_COMPLETE` when the target summary arrives and
aborts repeated brownout resets as `BROWNOUT_ABORT`. `TARGET_STRESS_SUMMARY: PASS`
is therefore not a claim that those
peripherals are physically verified.

### 3. HIL replay/parser contracts

Current CI executes replay/parser tests for:

- `tools/run_storage_spi_hil.ps1` / `tools/storage_spi_contract.ps1`;
- `tools/run_boot_responsiveness_hil.ps1` / `tools/boot_responsiveness_contract.ps1`.

Replay mode consumes `-LogPath` and deliberately returns a physical-HIL result of `BLOCKED` even when telemetry classification is `PASS`. Parser tests cover valid input, malformed/missing telemetry, threshold boundaries and failure markers. They never prove current-board timing, SPI2/DMA behavior, physical keypad debounce, TFT rendering, SD media/electrical behavior or real PSRAM timing.

### 4. Live-board HIL

Live runners require current target telemetry from a real serial port. Missing required telemetry must be `BLOCKED` or `FAIL`, never silently accepted.

Relevant tools:

- `tools/run_boot_responsiveness_hil.ps1` — programs through COM4, captures passive UART through COM6 and gates startup/recovery/keypad-event/priority telemetry.
- `tools/run_storage_spi_hil.ps1` — passive COM6 storage/SPI/recorder latency capture; it never flashes firmware.
- `tools/run_keypad_timing_hil.ps1` — opt-in ADC broker and timer-task timing profile.
- `tools/run_pipeline_hil.ps1` — broader measurement-pipeline verification and explicitly separated physical observations.

`tools/run_stress_hil.ps1` — target FreeRTOS/heap/ISR/rate-sweep stress run
with separate physical attestations for AD7606/SPI3, shared SPI2/SD, TFT,
Wi-Fi and I2C.

### 5. nRF54L15 test surface

The detailed future task is [`nrf54l15-test-task.md`](nrf54l15-test-task.md).
It is a separate test surface with its own build, software/native and physical
tiers. The current executable baseline command is:

```powershell
.\nRF54L15\pio.ps1 build
```

The wrapper is required because this checkout's Windows path contains spaces;
generated PlatformIO/Zephyr output stays in
`%USERPROFILE%\.platformio-workspaces\nrf54l15`.

The automatic no-flash test launch is:

```powershell
.\nRF54L15\pio.ps1 test
```

That action is restricted to suites named `test_sw_*`, which are reserved for
tests explicitly qualified as software/native and hardware-free. PlatformIO's
`--without-uploading` prevents upload but does not by itself guarantee that an
embedded suite will not need a physical serial/test port, so board-dependent
nRF tests are excluded from the automatic `Tests: All (no flash)` path.

The current `test_sw_upstream_protocol` suite is executable in the separate
native environment and is invoked explicitly with `nRF54L15/pio.ps1 -Action
test`. If the host has no GCC toolchain, the wrapper reports `BLOCKED`; it does
not silently claim a pass. The canonical NCS-native embedded boundary in
`Run All` remains `SKIPPED` until such a runner is reviewed. Do not report an
ESP32/WebUI-only result as nRF evidence.

The planned nRF layers are:

- build/configuration isolation and clean/incremental wrapper builds;
- host/native policy tests for identity, topology, logger latches, tilt,
  GY-BMI160/BME280 local sensor state and snapshot handling, ranging data and
  rotating Channel Sounding sessions;
- one-board HIL for boot/USB, optional remote LEDs, external GY-BMI160 on
  D11/D12 local I2C, local BME280 reads/snapshot freshness, and upstream I2C
  visibility;
- multi-node HIL for pairwise ranging, distance matrices, role rotation and
  missing/reconnected nodes;
- full ESP32 + TCA9548A + remote-node topology HIL.

No layer assumes that five physical boards are available in software-only CI.
A future host/native-only PlatformIO environment is allowed if required for
software testing, but it must never become a second production central/remote
firmware variant.

## Requirement gates

### Keypad / input reliability

Software evidence:

- raw ladder decode boundaries and ADC-service ownership;
- bounded `KEYPAD_RAW_TRANSITION` first-hop marker only on decoded state transitions;
- `KEYPAD_BUTTON_PRESS` on official Button `PRESS_DOWN`;
- `KEYPAD_EVENT_GATE`, `KEYPAD_EVENT_POST` and `KEYPAD_EVENT_DISPATCH` breadcrumbs;
- stale navigation older than the UI freshness boundary is discarded rather than replayed later.

Physical acceptance still requires all five real buttons to produce the expected end-to-end trace and visible TFT navigation, with no spontaneous safe shutdown during boot.

### Startup responsiveness

Software contracts require object recovery to run outside the interactive critical path and Web status-cache work to follow recovery. Live acceptance requires:

- `startup_total <= 5000 ms` on the populated test card;
- bounded recovery storage windows;
- no stale keypad navigation caused by SD stalls;
- no spontaneous safe shutdown or panic/watchdog/brownout/assert marker.

### Storage/SPI2 and recorder latency

`STORAGE_SPI_TIMING` and `RECORDER_IO_TIMING` are numerical telemetry, but synthetic replay thresholds are not a physical baseline. Live performance acceptance requires thresholds established from a recorded real-board run including firmware SHA, date, SD card, SDSPI frequency, sample rate/channel configuration and workload. The same run must correlate latency with acquisition health and recorder dropped frames.

Do not migrate to SDMMC or another storage interface merely because SDSPI is single-block. At the current recorder data rate, measured latency and dropped-frame evidence must demonstrate a sustained-throughput bottleneck before an interface migration is justified.

### Runtime memory / task observability

Host policy tests cover deterministic PSRAM-preferred allocation, explicit internal-RAM fallback, total allocation failure and top-N ordering/equality. Real memory-floor acceptance requires live `MEM_RUNTIME` data under the intended workload; host allocation fakes do not prove physical fragmentation or PSRAM timing.

### Power shutdown

Host tests cover early-boot shutdown disarm, explicit post-startup arming, held-button release policy and authoritative software latch behavior. Physical acceptance requires an intentional post-startup GPIO21 long press to perform ordered safe shutdown and, where timing is claimed, a real raw GPIO21-edge to latch measurement. A software debounce marker is not that physical measurement.

## SPI2 contention scenarios

The detailed physical scenarios remain in [`spi2-contention-hil.md`](spi2-contention-hil.md). The decisive recorder scenario requires healthy acquisition, normal recorder completion and `dropped_frames == 0`; a multi-second network/storage operation must not monopolize the shared TFT/SD SPI2 path.

## Keypad timing profile

See [`keypad-adc-timing-hil.md`](keypad-adc-timing-hil.md). It measures broker-owned keypad ADC snapshot duration and shared ESP timer-task lateness. Performance thresholds remain board-derived, not invented from replay fixtures.

## CI acceptance

Before merging firmware/test changes, the normal `main` verification must pass:

1. WebUI dependencies/typecheck/Console/XR tests;
2. native host manifest;
3. storage/SPI and boot HIL replay/parser contracts;
4. startup/storage/runtime source contracts;
5. production Web asset build/optimization checks;
6. ESP-IDF 5.5.4 production firmware build;
7. isolated nRF54L15 wrapper build and the qualified automatic nRF
   software/native test launch; the canonical NCS-native boundary remains
   explicit `SKIPPED`, while the separate PlatformIO native suite is invoked
   only by its dedicated task.

A fully green run means the checked software contracts for the executed stages
pass. A current ESP32/WebUI-only result does **not** close the nRF task, and no
green software run closes a task whose definition explicitly requires
live-board evidence. Physical GY-BMI160/BME280 wiring and power, remote LEDs
and Bluetooth Channel Sounding always require target/HIL evidence.

## Evidence retention

For each physical HIL run keep the complete UART log and record firmware SHA, board, date, SD card, SDSPI frequency, sample rate/channel configuration, object/session population, runner thresholds, maximum measured values, acquisition-health markers, recorder `dropped_frames`, and any relevant manual observation. For nRF HIL also record plain-XIAO board identity, GY-BMI160/BME280 modules and wiring when present, local `D11/P0.03`/`D12/P0.04` bus, `3V3_OUT` power and RJ45 pin-2 NC check, node role/topology, Channel Sounding role/session details and relevant platform/framework revisions. If hardware/configuration materially changes, re-establish the baseline instead of reusing old thresholds silently.
