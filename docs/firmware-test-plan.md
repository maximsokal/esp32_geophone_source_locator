# Firmware test plan and audit (2026-09-08)

Scope: existing C firmware on main, starting at `844437f93306e5d4182dc8129162900008a5de0f`, initially clean. CI/CD remains untouched and no commits, push, branches, reset, or stash are used. Hardware actions are isolated to the explicitly invoked ESP tasks.

## Component map

- `main` orders boot, service shutdown, recorder finalization and power-off.
- `board_config` owns wiring; `app_config/app_task_config.h` owns task/core policy.
- `ad7606_driver` owns dedicated SPI3 conversion/BUSY/readout; `ad7606_acquisition` runs on Core 1. `seismic_acquisition` selects AD7606, ESP ADC bench, or synthetic backend.
- `seismic_buffer` publishes 4-channel frames to independent cursor consumers. UI, recorder and web must not consume one another's data.
- `measurement_service` queues commands on Core 0; `seismic_recorder` drains frames and finalizes session files. UI storage commands remain outside LVGL callbacks.
- `storage_manager` serializes operations and DMA reserve; `spi_bus` quiesces TFT on shared SPI2; `sd_card` owns mount lifecycle. `object_format`, `object_recovery`, `object_store` and `object_service` own persisted object contracts and recovery.
- `localization` is a 4..8-sensor 3D TDOA solver, not an event detector or validated end-to-end physical locator.
- `adc_service` owns ADC1 and channel settling; `keypad` interprets the analog ladder. `ina3221_monitor` publishes battery/system/AD7606 rail telemetry.
- `display` owns panel hardware; `ui` owns LVGL. `wifi_manager` and `web_server` serve local HTTPS and flash-authoritative assets. `device_status` aggregates snapshots.
- `power_control` monitors button and requests power cut only after application shutdown.

## Initial executable surface

Four standalone host C suites: AD7606 presence policy, INA3221 policy, web status policy, UI measurement trace. A file called `seismic_source_test.c` is a production synthetic backend, not an automated unit test. No root host runner, Unity test app or pytest suite existed.

Existing Web tests: console 24 tests (6 files); XR 582 tests (95 files). Local baseline: console PASS; XR 581 PASS / 1 FAIL because `meshQualityUi.test.ts` hard-codes an English thousands separator while Windows Node uses uk-UA. Setting LANG/LC_ALL does not change this Windows ICU default.

Production build binding: ESP-IDF 5.5.4, Python 3.11.2 at `C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe`, same interpreter in `build/CMakeCache.txt`. Python 3.13 IDF environment from historical attachment is absent. CMake 3.30.2, Ninja 1.12.1, Xtensa GCC esp-14.2.0_20260121. Initial `idf.py build` exited 2 with a third-party GCC internal compiler error in `esp_lcd_panel_rgb.c:700`; a bounded retry with the same toolchain and `ninja -C build -j 2` passed.

## Current executable surface

- The manifest-driven native runner builds and executes 21 isolated C suites covering AD7606 policy, DMA and cadence boundaries, runtime task observability, INA3221 conversion, keypad, localization, measurement service, object format/store/recovery, seismic buffer/recorder, storage manager, SPI arbitration, power-button policy, web status/flash serving, UI trace reduction and the staged seismic recorder/web pipeline.
- `test_app` is an ESP-IDF 5.5.4 Unity target application with four deterministic policy cases. Its ESP32-S3 build passed; it runs all cases automatically when flashed and emits `TEST_APP_RESULT` for the COM6 monitor.
- `tests/python` provides deterministic manifest, task and pipeline-log replay checks plus one explicitly gated HIL check. The default invocation runs the deterministic checks and skips HIL.
- `tools/run_pipeline_hil.ps1` is the explicit production Measurements-pipeline gate. It combines COM4 flash, passive COM6 logs, optional HTTPS API checks, runtime heartbeat/progress checks for acquisition, recorder, UI, keypad and power-button loops, and a device-monotonic `runtime.now_ms` age gate (`-MaxTaskHeartbeatAgeMs`, default 1000 ms). It also performs explicit recorder start/stop and completed-session metadata validation; it reports unautomatable TFT/keypad/power-button checks as `BLOCKED` when `-RequireFullPipeline` is requested, or records them as `PASS` only when the live operator supplies all three explicit confirmation switches after observing the target. Those switches themselves require `-RequireFullPipeline`.
- WebUI remains a separate baseline: the locale-dependent XR assertion now derives the expected thousands separator from `toLocaleString()`. The console seismic contract validates `/api/seismic` normalization and carries a signed impulse into the waveform polyline helper; live Wi-Fi, browser layout and physical pixel output remain HIL-only.
- `.vscode/tasks.json` provides separate local test, build, flash, and monitor tasks. `Tests: All (no flash)` has no hardware dependency; the Unity HIL task checks live COM4/COM6 before flashing, while standalone flash/monitor tasks check only their matching port.

No CI/CD workflow was added or run, per the user instruction. The host runner and local commands are intentionally executable from a developer checkout.

## Risk-based matrix

Existing coverage describes the initial checkout; additions and verified results belong in the final report. P0 is ordered by data loss, real-time integrity, then diagnostic/control behavior.

| Component | Risk | Test type | Existing coverage | Missing / planned cases | Priority |
| --- | --- | --- | --- | --- | --- |
| measurement_service | blocking UI, lost recording, invalid state | host worker with queue/recorder fakes | none | LIVE/STARTING/RECORDING/STOPPING/REVIEW/KEEP/DELETE, errors, busy/full queue, shutdown | P0 |
| seismic_buffer | mixed frames, cursor theft, dropped data | actual C host + target concurrency | none | empty/capacity/wrap/overwrite, independent consumers, generation reset, publication | P0 |
| seismic_recorder | corrupt or lost sessions | host temp FS / boundary fakes | none | header bytes, IDs, state validation, write/finalize/drop errors, KEEP/delete | P0 |
| storage_manager | SD DMA OOM and lock leakage | host capability/SPI/FS fakes | none | reserve/nesting/512-byte contiguous threshold, begin/end/error symmetry, atomic writes | P0 |
| object_format | invalid IDs/schema | host pure C | none | schema defaults, bounds, invalid numbers/IDs | P0 |
| object_recovery | data loss on interrupted write | host fake/temp FS | none | valid/corrupt/missing main, backup and temp priorities, repair errors | P0 |
| object_store / object_service | wrong object mutation, traversal | host boundaries/temp FS | none | create/list/active/delete, duplicate IDs, paths and limits | P0 |
| localization | incorrect XYZ/confidence | independent synthetic C vectors | none | 4..8 sensors, inside/outside/noise/offset, invalid/degenerate geometry, residual/convergence | P0 |
| ad7606_driver | false presence, wrong sample timing/scale | host policy + mocked driver; later HIL | presence policy | all eight raw words, BUSY transitions/stuck/timeout, SPI failure, CONVST timestamp, flat diagnostics | P0 |
| ui measurement | lost spikes/unsafe navigation | host pure reducer/state | peak reducer | signed spikes/boundaries/short/empty/constants, LEFT while busy and KEEP semantics | P0 |
| spi_bus | TFT/SD lock inversion | host callback/RTOS fakes | none | quiesce/resume symmetry, missing guards, failure unlock, nesting contract | P0 |
| web flash send | TLS memory pressure/truncated assets | host HTTP send fakes | status policy | 0/1/2048/2049 and multiples, final chunk, headers/close/error propagation | P0 |
| power_control / main shutdown | premature power cut | host ordered calls, no physical output | none | debounce/sample rounding/cancel, finalize-before-LVGL and acquire-stop order, errors | P0 |
| INA3221 | wrong power classification | host conversions/policy | battery/source policy | register sign/zero/extrema, threshold validity | P0 |
| keypad | unintended input | host decode/poll fake | none | all buttons and thresholds, settling/noise/debounce | P0 |
| acquisition orchestrator / ADC service | stale state or wrong backend | host fake backend/ADC | none | failed start cleanup, restart, settling/read errors | P1 |
| Measurements pipeline (acquisition -> buffer -> recorder/UI/web) | wrong rate, lost frames, blocked service tasks | 21-suite host contracts plus explicit production HIL runner | staged host fan-out, recorder/web parity and runtime heartbeat/progress telemetry | physical SPI DMA timing, SD media, TFT pixels, Wi-Fi transport and live input responsiveness | P0 |
| display / Wi-Fi / device_status | runtime coupling and memory pressure | local production build, targeted unit seams; HIL | web status policy | lifecycle error paths, real heap and DMA fragmentation | P1 |

## Verification boundaries

HOST TESTED proves selected C behavior against deterministic boundary fakes. COMPILES proves link/build integration. Neither proves actual dual-core publication, DMA fragmentation, SPI signal integrity, AD7606 flat-data recovery, 500 Hz jitter, or safe power-off on the board. TARGET/HIL/PHYSICALLY VERIFIED remain separate; the current HIL attempt is blocked by missing live serial ports.

## Verification results

- Host runner: default native MSVC and explicit LLVM-MinGW Clang both completed 21/21 suites. Clang with AddressSanitizer and UndefinedBehaviorSanitizer also completed 21/21; a separate Clang coverage-instrumented run completed 21/21 (instrumentation verified, no percentage gate claimed).
- Python: `python -m pytest -q` completed with 21 passed and 1 skipped (the explicit hardware HIL gate).
- Target test app: the task helper completed an ESP32-S3 build after selecting that target; four Unity test cases compiled and linked, with automatic all-tests execution wired for HIL.
- Production firmware: the initial parallel `idf.py build` reproduced the third-party compiler failure described above; `ninja -C build -j 2` then completed with the unchanged ESP-IDF toolchain. This is a compile result, not a flash or hardware result.
- WebUI: console tests passed; XR tests passed after making the number-format assertion locale-aware.
- Production pipeline replay: a valid captured AD7606 log returned `PIPELINE_HIL_RESULT: PASS`; a log containing an AD7606 read timeout returned `PIPELINE_HIL_RESULT: FAIL`. These parser checks use no serial port and never flash. A live full-pipeline invocation remains `BLOCKED` until COM4, COM6, HTTPS base URL, an existing object ID and the three physical observations are available.
- Serial preflight: the requested COM4/COM6 names currently appear only as phantom PnP entries (`CM_PROB_PHANTOM`/`Unknown`); `SerialPort.GetPortNames()` returns no live ports and both `mode COM4` and `mode COM6` report `Illegal device name`. The HIL task therefore stops before flash until the board and passive adapter are connected.

## Optimization gate

First establish green firmware host contracts, then benchmark a specific eligible hotspot using repeated samples and report before/after with identical inputs/toolchain. No changes to timers, pins, binary format, task/core assignments, calibration, or hardware thresholds on speculation. Physical memory/jitter optimizations await real telemetry. Keep diagnostic markers.

The current pass made testability and contract fixes only. No speculative runtime optimization was applied because no board telemetry or reproducible hotspot measurement was available.
