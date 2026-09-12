# Firmware test plan and audit (2026-09-08)

Scope: existing C firmware on main, starting at `844437f93306e5d4182dc8129162900008a5de0f`, initially clean. CI/CD remains untouched and no commits, push, branches, reset, or stash are used. Hardware actions are isolated to the explicitly invoked ESP tasks.

## Component map

- `main` orders boot, service shutdown, recorder finalization and power-off.
- `board_config` owns wiring; `app_config/app_task_config.h` owns task/core policy.
- `ad7606_driver` owns dedicated SPI3 conversion/BUSY/readout; `ad7606_acquisition` runs on Core 1. `seismic_acquisition` selects AD7606, ESP ADC bench, or synthetic backend.
- `seismic_buffer` publishes 4-channel frames to independent cursor consumers. UI, recorder and web must not consume one another's data.
- `measurement_service` queues commands on Core 0; `seismic_recorder` drains frames and finalizes session files through an asynchronous `DRAIN -> FLUSH -> SYNC -> CLOSE -> METADATA -> COMPLETE/ERROR` worker state machine. UI storage commands remain outside LVGL callbacks.
- `storage_manager` serializes operations and DMA reserve; `spi_bus` quiesces TFT on shared SPI2; `sd_card` owns mount lifecycle. `object_format`, `object_recovery`, `object_store` and `object_service` own persisted object contracts and recovery.
- `localization` is a 4..8-sensor 3D TDOA solver, not an event detector or validated end-to-end physical locator.
- `adc_service` owns ADC1 and channel settling; `keypad` owns five official Button core objects with custom `button_driver_t` adapters, so every KS0466 ladder read goes through the service and its private ADC1 handle. Timer callbacks use the non-blocking broker API, retain the last decoded state on `ESP_ERR_TIMEOUT`, and timestamp the shared cache after a completed read. `ina3221_monitor` publishes battery/system/AD7606 rail telemetry through the shared `sensor_bus` I2C0 owner.
- `display` owns panel hardware; `ui` owns LVGL. `wifi_manager` and `web_server` serve local HTTPS and flash-authoritative assets. `device_status` aggregates snapshots.
- `app_events` owns the dedicated ESP-IDF `esp_event` user loop; UI, Wi-Fi and Web subscribers/publishers use its preserved event IDs. `power_control` uses the official GPIO Button adapter and latches shutdown through the EventGroup before any best-effort notification.
- `status_indicator` consumes explicit status decisions only; its `led_indicator` backend is disabled because this board has no connected status LED GPIO contract. `insights_bridge` is optional, exports selected health metrics only, and retries from a dedicated low-priority worker rather than inside the shared APP_EVENT handler. `seismic_dsp` is a synchronous, task-free measured-signal foundation with no invented coefficients.

## Initial executable surface

Four standalone host C suites: AD7606 presence policy, INA3221 policy, web status policy, UI measurement trace. A file called `seismic_source_test.c` is a production synthetic backend, not an automated unit test. No root host runner, Unity test app or pytest suite existed.

Existing Web tests: console 24 tests (6 files); XR 582 tests (95 files). Local baseline: console PASS; XR 581 PASS / 1 FAIL because `meshQualityUi.test.ts` hard-codes an English thousands separator while Windows Node uses uk-UA. Setting LANG/LC_ALL does not change this Windows ICU default.

Production build binding: ESP-IDF 5.5.4, Python 3.11.2 at `C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe`, same interpreter in `build/CMakeCache.txt`. Python 3.13 IDF environment from historical attachment is absent. CMake 3.30.2, Ninja 1.12.1, Xtensa GCC esp-14.2.0_20260121. Initial `idf.py build` exited 2 with a third-party GCC internal compiler error in `esp_lcd_panel_rgb.c:700`; a bounded retry with the same toolchain and `ninja -C build -j 2` passed.

## Current executable surface

- The manifest-driven native runner builds and executes 39 isolated C suites covering APP_EVENT delivery, AD7606 policy, DMA and cadence boundaries, runtime task observability, INA3221 conversion, keypad compatibility, ADC1 ownership and timing-statistics boundaries, ADC1 settling, seismic DSP, localization, measurement service, object format/store/recovery, seismic buffer/recorder, storage manager, SPI arbitration, power-button policy, web status/flash serving, UI trace reduction and the staged seismic recorder/web pipeline.
- `tests/target/test_app` is an ESP-IDF 5.5.4 Unity target application with four deterministic policy cases. Its ESP32-S3 build passed; it runs all cases automatically when flashed and emits `TEST_APP_RESULT` for the COM6 monitor.
- `tests/python` provides deterministic manifest, task, pipeline-log and keypad-timing replay checks plus one explicitly gated HIL check. The default invocation runs the deterministic checks and skips HIL.
- `tools/run_pipeline_hil.ps1` is the explicit production Measurements-pipeline gate. It combines COM4 flash, passive COM6 logs, optional HTTPS API checks, runtime heartbeat/progress checks for acquisition, recorder and UI, and a device-monotonic `runtime.now_ms` age gate (`-MaxTaskHeartbeatAgeMs`, default 1000 ms). The official Button component owns input sampling/debounce; physical button delivery, startup-held behavior, GPIO18 pulse and TFT response remain explicit HIL observations. The power adapter also reports `POWER_BUTTON_TIMING` for debounced-down to authoritative latch timing, but that marker is not a physical-edge measurement. The configured 150 ms Button long-press value and debounced release event are software configuration/evidence only. Do not infer raw timing from them: measure `GPIO21 HIGH` to the shutdown latch on the board and pass the measured value, target and tolerance with `-ConfirmPowerButtonTiming`, `-PowerButtonTimingMs`, `-PowerButtonTimingTargetMs` and `-PowerButtonTimingToleranceMs`. It also performs explicit recorder start/stop and completed-session metadata validation; it reports unautomatable TFT/keypad/power-button checks as `BLOCKED` when `-RequireFullPipeline` is requested, or records them as `PASS` only when the live operator supplies the explicit confirmation switches after observing the target. Those switches themselves require `-RequireFullPipeline`.
- `tools/run_keypad_timing_hil.ps1` is a separate opt-in timing gate documented in `docs/keypad-adc-timing-hil.md`. It builds isolated AD7606 and ESP ADC bench profiles, measures the broker-owned keypad ADC snapshot duration as `min/avg/p95/max`, and measures shared `ESP_TIMER_TASK` dispatch lateness/overruns. Its result is telemetry validity only; `PERFORMANCE_ACCEPTANCE` remains `NOT_ESTABLISHED` until real-board measurements define thresholds. It does not replace the full production pipeline HIL.
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
| power_control / main shutdown | premature power cut | host ordered calls, no physical output | policy and debounced timing marker | debounce/sample rounding/cancel, raw GPIO21 HIGH -> shutdown latch measurement, finalize-before-LVGL and acquire-stop order, errors | P0 |
| INA3221 | wrong power classification | host conversions/policy | battery/source policy | register sign/zero/extrema, threshold validity | P0 |
| keypad / official Button adapter | unintended or repeated input | host ladder compatibility + ADC ownership regression + target compile; later HIL | raw decode, broker routing and API compatibility | all five physical buttons, ADC1 ownership, measured raw ladder boundaries, settling/noise/debounce, startup-held input, ADC snapshot duration and shared timer-task lateness under AD7606/ESP ADC bench load | P0 |
| app_events / UI dispatch | blocked service loop or lost UI action | host event contract + source contract + target build | event IDs and zero-wait post | queue pressure, bounded UI pending queue, LVGL-owner application, startup/shutdown subscriber ordering | P0 |
| power Button / safe shutdown | power cut before persistence | host policy + target build; later HIL | policy, EventGroup latch and debounced timing marker | long-press/release, startup-held button, queue-full notification, raw GPIO21 HIGH -> shutdown latch target/tolerance, GPIO18 200 ms pulse, ordered shutdown | P0 |
| sensor_bus / INA3221 | I2C ownership conflict or bad telemetry | host/source contract + target build; later HIL | conversion/policy | one I2C0 owner, GPIO9/10, 100 kHz, INA probe/averaging and TCA9548A measured topology | P0 |
| status_indicator / Insights | misleading state or data leakage | source contract + target build | explicit status semantics | physical LED wiring, sticky state over power cycle, optional cloud/auth path, measured overhead and privacy audit | P1 |
| seismic_dsp foundation | unvalidated signal alteration | host API contract + target build | no-op API and stage names | measured AD7606/ZF-24HP model, coefficients, numerical regression before enabling stages | P1 |
| acquisition orchestrator / ADC service | stale state or wrong backend | host fake backend/ADC | none | failed start cleanup, restart, settling/read errors | P1 |
| Measurements pipeline (acquisition -> buffer -> recorder/UI/web) | wrong rate, lost frames, blocked service tasks | 38-suite host contracts plus explicit production HIL runner | staged host fan-out, recorder/web parity, runtime heartbeat/progress telemetry and power-button timing marker | physical SPI DMA timing, SD media, TFT pixels, Wi-Fi transport, live input responsiveness and raw GPIO21 timing | P0 |
| display / Wi-Fi / device_status | runtime coupling and memory pressure | local production build, targeted unit seams; HIL | web status policy | lifecycle error paths, real heap and DMA fragmentation | P1 |

## Verification boundaries

HOST TESTED proves selected C behavior against deterministic boundary fakes. COMPILES proves link/build integration. Neither proves actual dual-core publication, DMA fragmentation, SPI signal integrity, AD7606 flat-data recovery, 500 Hz jitter, or safe power-off on the board. The debounced `POWER_BUTTON_TIMING` marker, including `debounced_elapsed_ms`, is software evidence only; it does not replace a raw `GPIO21 HIGH` to shutdown-latch measurement. TARGET/HIL/PHYSICALLY VERIFIED remain separate; the current HIL attempt is blocked by missing live serial ports.

## Verification results

- Host runner: default native MSVC and explicit LLVM-MinGW Clang both completed 21/21 suites. Clang with AddressSanitizer and UndefinedBehaviorSanitizer also completed 21/21; a separate Clang coverage-instrumented run completed 21/21 (instrumentation verified, no percentage gate claimed).
- Python: `python -m pytest -q` completed with 21 passed and 1 skipped (the explicit hardware HIL gate).
- Target test app: the task helper completed an ESP32-S3 build after selecting that target; four Unity test cases compiled and linked, with automatic all-tests execution wired for HIL.
- Production firmware: the initial parallel `idf.py build` reproduced the third-party compiler failure described above; `ninja -C build -j 2` then completed with the unchanged ESP-IDF toolchain. This is a compile result, not a flash or hardware result.
- WebUI: console tests passed; XR tests passed after making the number-format assertion locale-aware.
- Production pipeline replay: a valid captured AD7606 log returned `PIPELINE_HIL_RESULT: PASS`; a log containing an AD7606 read timeout returned `PIPELINE_HIL_RESULT: FAIL`. These parser checks use no serial port and never flash. A live full-pipeline invocation remains `BLOCKED` until COM4, COM6, HTTPS base URL, an existing object ID and the three physical observations are available.
- Serial preflight: the requested COM4/COM6 names currently appear only as phantom PnP entries (`CM_PROB_PHANTOM`/`Unknown`); `SerialPort.GetPortNames()` returns no live ports and both `mode COM4` and `mode COM6` report `Illegal device name`. The HIL task therefore stops before flash until the board and passive adapter are connected.

### Current migration verification (2026-09-12)

- HOST TESTED: `python tools/run_host_tests.py --keep-going` passed 36/36 native C suites.
- HOST TESTED: `python -m pytest -q` passed 70 tests with 1 explicitly skipped hardware HIL gate.
- HOST TESTED: Web Console passed 38/38, Web XR passed 582/582, and Web typecheck passed.
- COMPILES: the Unity target build produced `geolocator_test_app.bin` at `0x29fd0` with 84% app-partition free.
- COMPILES: the clean production build in `build-refactor-final` passed after a bounded `-j2` retry; the image is `0x1ff640`, app free space is `0x2009c0` (50%), and the bootloader has 36% free space.
- HIL TESTED: SKIPPED. Serial preflight found no live COM4/COM6 ports, so no flash, UART observation, runtime heap/HWM capture, or physical input/sensor/display/storage verification was performed.
- PHYSICALLY VERIFIED: not claimed. The physical acceptance matrix remains pending board connection and live observations.

### Follow-up objective verification (2026-09-12)

- HOST TESTED: `python tools/run_host_tests.py --keep-going` passed 37/37 native C suites, including ADC1 external-borrower settling and power-button timing policy coverage.
- HOST TESTED: `python -m pytest -q` passed 74 tests with 1 explicitly skipped hardware HIL gate.
- COMPREHENSIVE LOCAL SUITE: `powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_suite.ps1 -Suite All -ProductionBuildDir build-refactor-final` exited 0; Host C, Python, Web Console, Web XR, Web typecheck, ESP Unity build and ESP production build all passed, while the explicit Unity HIL step was skipped.
- COMPILES: baseline `powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_idf.ps1 -Action build -ProjectPath . -BuildDir build-refactor-final -Jobs 2` exited 0. The image is `0x1ff750`, app free space is `0x2008b0` (50%), and the bootloader is `0x5160` with 36% free. `idf.py size` reported 2,094,809 bytes of unpadded image content.
- COMPILES: opt-in `powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_insights_profile.ps1 -BuildDir build-refactor-insights -Jobs 2` exited 0 with an isolated build `sdkconfig` explicitly enabling `CONFIG_APP_ESP_INSIGHTS=y`, metrics and the test auth provider while keeping heap/Wi-Fi metrics and variables disabled. The image is `0x20ec50`, app free space is `0x1f13b0` (49%), and `idf.py size` reported 2,157,525 bytes of unpadded image content.
- COMPILES: `idf.py size-components` completed with exit 0 for both `build-refactor-final` and `build-refactor-insights`; no runtime heap/HWM conclusion is inferred from static size output.
- HIL TESTED: SKIPPED. The corrected COM4/COM6 preflight found no live serial ports; no flashing, UART marker capture, runtime heartbeat/HWM, or physical timing measurement was performed.
- PHYSICALLY VERIFIED: not claimed. The `POWER_BUTTON_TIMING` log marker proves only debounced software timestamps. Acceptance still requires measuring raw `GPIO21 HIGH` to the authoritative shutdown latch on the board and supplying explicit measured value, target and tolerance to the HIL runner.

### ADC1 ownership correction verification (2026-09-12)

- HOST TESTED: `python tools/run_host_tests.py --keep-going` passed 38/38 native C suites. The `keypad_adc_ownership` suite creates all five official Button-core objects, drives their custom level callbacks, verifies one shared four-sample `adc_service_read_average_raw()` read per timer window, and records zero direct `adc_oneshot_read()` calls from the keypad path.
- HOST TESTED: `python -m pytest -q tests/python/test_task_liveness_contract.py` passed 4/4; source contracts reject `button_adc.h`, `iot_button_new_adc_device`, direct `adc_oneshot_read`, and the public borrowed ADC handle in `keypad_button_adapter.c`.
- HOST TESTED: the keypad ownership suite also covers a busy ADC broker: the timer-safe try-read returns `ESP_ERR_TIMEOUT` without touching ADC1, the previous decoded state is retained, later logical Button handles do not retry inside the cache window, and a delayed successful read timestamps the cache after completion.
- HOST TESTED: the Insights source contract requires atomic cross-context state publication (`atomic_bool`, acquire loads and release stores) for the retry worker and public status readers.
- HOST TESTED: `powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_suite.ps1 -Suite All -ProductionBuildDir build-refactor-final` exited 0 after the final correction: Host C 38/38, Python 74 passed plus 1 skipped, Web Console 38/38, Web XR 582/582, Web typecheck, Unity build, and production build passed. Unity HIL remained skipped.
- COMPILES: `powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_idf.ps1 -Action build -ProjectPath . -BuildDir build-refactor-final -Jobs 2` exited 0 after the ADC API change. The production image is `0x1fdd30` with `0x2022d0` (50%) app-partition free; `idf.py size` reported 2,088,121 bytes of unpadded image content, and `idf.py size-components` exited 0.
- COMPILES: the isolated opt-in Insights profile also exited 0 with `CONFIG_APP_ESP_INSIGHTS=y`, the test auth provider, and the privacy flags enabled as specified. Its image is `0x20d360` with `0x1f2ca0` (49%) app-partition free; `idf.py size` reported 2,151,153 bytes and `idf.py size-components` exited 0. Its APP_EVENT connection handler only notifies the dedicated retry worker; `esp_insights_init()` is not called from the shared event loop.
- HIL TESTED: SKIPPED. Serial preflight found no live COM4/COM6 ports, so no target flash, UART observation, runtime HWM capture, or physical ADC/button verification was performed.
- PHYSICALLY VERIFIED: not claimed. The ownership proof is host/static plus compile-time only; live GPIO8/ADC1 behavior still requires the connected board.

### Timer-safe ADC and atomic-state verification (2026-09-12)

- HOST TESTED: `python tools/run_host_tests.py --keep-going` passed 38/38; the keypad contention regression verifies zero-wait broker acquisition, state retention on `ESP_ERR_TIMEOUT`, one attempt per cache window, and post-read cache timestamping.
- HOST TESTED: `python -m pytest -q` passed 74 tests with 1 explicitly skipped hardware HIL gate; the architecture/liveness contracts require the non-blocking keypad path and C11 atomic Insights state publication.
- COMPILES: the production rebuild passed with image `0x1fddf0`, app free space `0x202210` (50%), and `idf.py size` total 2,088,317 bytes; `idf.py size-components` exited 0.
- COMPILES: the isolated Insights rebuild passed with `CONFIG_APP_ESP_INSIGHTS=y`, image `0x20d450`, app free space `0x1f2bb0` (49%), and `idf.py size` total 2,151,389 bytes; `idf.py size-components` exited 0. The enabled `insights_bridge.c` branch compiled with `atomic_bool` and acquire/release operations.
- HIL TESTED: SKIPPED. No live COM4/COM6 ports were available; no flash, UART, target timing/latency, heap/HWM, or physical ADC/button verification was performed.
- PHYSICALLY VERIFIED: not claimed. Host contention timing and target compilation do not prove actual `ESP_TIMER_TASK` latency or GPIO8 ladder behavior.

## Optimization gate

### Keypad timing watchlist verification (2026-09-12)

- HOST TESTED: `python tools/run_host_tests.py --keep-going` passed 39/39 native C suites, including the pure histogram boundary regression for valid `2047`-us p95, true `>=2048`-us overflow censoring, and a valid p95 with a small overflow tail.
- HOST TESTED: the opt-in timing contract and replay tests passed 11/11. Replay coverage includes AD7606, ESP ADC bench at 500 Hz, the 2047-us normal p95 boundary, valid p95 with a small overflow tail, censored histogram p95 data, non-zero ADC-error rejection, malformed percentile data, and separation of telemetry validity from physical keypad attestation.
- COMPILES: the isolated AD7606 timing profile built with `CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS=y`; final cached Ninja `-j2` retry exited 0 after the unchanged ESP-IDF GCC ICE retry, image `0x1fe7f0`, app free space `0x201810` (50%), and `idf.py size` total 2,090,881 bytes. Its live runner stopped at serial preflight because no live COM4/COM6 ports were present; `idf.py size-components` exited 0.
- COMPILES: the isolated ESP ADC bench timing profile built with `CONFIG_APP_KEYPAD_TIMING_DIAGNOSTICS=y`, `CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH=y` and `CONFIG_SEISMIC_ADC_BENCH_SAMPLE_RATE_HZ=500`; image `0x1fc9d0`, app free space `0x203630` (50%), and `idf.py size` total 2,083,161 bytes. Its live runner also stopped at the same serial preflight gate; `idf.py size-components` exited 0.
- HIL TESTED: SKIPPED. No keypad ADC `min/avg/p95/max` or shared timer-task lateness values were captured from the board. The dedicated runner is ready for three separate live windows: AD7606 idle, AD7606 with frequent keypad presses, and ESP ADC bench at 500 Hz with frequent keypad presses.
- PHYSICALLY VERIFIED: not claimed. The timing profile's replay and target builds prove the measurement path only; they do not prove ADC polling duration, timer jitter or callback overruns on the physical ESP32-S3.

The timing profile remains outside `Suite All` because it is intentionally a
hardware/load measurement. See `docs/keypad-adc-timing-hil.md` for the exact
commands and the interpretation of valid versus censored bounded-histogram p95.

First establish green firmware host contracts, then benchmark a specific eligible hotspot using repeated samples and report before/after with identical inputs/toolchain. No changes to timers, pins, binary format, task/core assignments, calibration, or hardware thresholds on speculation. Physical memory/jitter optimizations await real telemetry. Keep diagnostic markers.

The current pass made testability and contract fixes only. No speculative runtime optimization was applied because no board telemetry or reproducible hotspot measurement was available.
