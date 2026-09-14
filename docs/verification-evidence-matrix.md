# Verification evidence matrix

This document is the authoritative boundary between automated software evidence and physical board proof. Historical counts and snapshots in `docs/firmware-test-plan.md` remain audit history; when they conflict with this file or current local verification, use this file plus the current repository state as the source of truth.

## Evidence classes

| Class | What runs | What it proves | What it does **not** prove |
| --- | --- | --- | --- |
| **HOST C** | Native C suites from `tests/host/manifest.json` and `tests/host/manifest.d/*.json`, compiled against deterministic ESP-IDF/FreeRTOS/component fakes | Pure/stateful C behavior, error paths, lifecycle policy, queue/lock ownership contracts and UI navigation policy that the suite actually exercises | ESP32 timing, DMA, dual-core scheduling, physical GPIO/I2C/SPI, SD electrical behavior, TFT pixels |
| **PYTHON SOURCE/ARCHITECTURE CONTRACT** | `tests/python` checks that inspect source/configuration/layout or deterministic text artifacts | Required architecture/configuration remains present; forbidden regressions are absent | Runtime behavior, FreeRTOS timing, peripheral behavior, physical device state |
| **NRF BUILD** | `nRF54L15/pio.ps1 build` through the isolated PlatformIO + Zephyr project | nRF source/configuration compiles and links for the selected Seeed board/toolchain | Runtime behavior, USB logging, GY-BMI160/GPIO/I2C/radio behavior, physical topology |
| **NRF NATIVE/UNIT** | `nRF54L15/pio.ps1 test` through the path-safe wrapper, restricted to explicitly qualified `test_sw_*` software/native suites; currently an explicit `SKIPPED` because none exist | Node state, topology policy, LED latches, tilt mapping, ranging validation and scheduling contracts that those tests exercise | XIAO GPIO, external GY-BMI160, I2C/radio electrical behavior or multi-board timing |
| **HIL REPLAY/PARSER** | PowerShell HIL runner invoked with `-LogPath`; Python tests replay saved/synthetic UART text through the same parser | Parser correctness, threshold boundaries, malformed/missing telemetry classification, failure-marker classification | Current board behavior. Replay never proves DMA, SPI2 arbitration, scheduling, keypad debounce, TFT rendering, SD media/electrical behavior, real PSRAM timing or current wiring |
| **LIVE BOARD HIL** | Runner opens the actual passive UART log port and observes telemetry produced by the connected target during the requested scenario | Only the machine-observable properties explicitly gated by that runner on that physical run | Uninstrumented physical behavior; visual/manual observations unless independently measured and encoded in telemetry |
| **NRF SINGLE-BOARD HIL** | Future dedicated runner on one physical plain XIAO nRF54L15; remote profile additionally includes external GY-BMI160 and LEDs | Target boot/USB, optional remote LED, local GY-BMI160 I2C/orientation, upstream I2C target and single-node lifecycle checks explicitly gated by the run | Multi-node ranging, TCA9548A visibility, role rotation across nodes and full ESP32 wiring |
| **NRF MULTI-NODE/FULL-TOPOLOGY HIL** | Future runner with two or more nRF boards, or the complete ESP32 + TCA9548A wiring | Pairwise ranging, rotating initiator roles, matrix/missing-node behavior and complete topology checks explicitly gated by the run | Any property not instrumented/observed in that topology; one-node HIL cannot stand in for it |
| **MANUAL PHYSICAL ATTESTATION** | Operator observes a physical effect and supplies an explicit confirmation switch where a runner supports it | The operator states that one named physical observation was seen in that run | It is not an automated measurement and must never be reported as equivalent to a telemetry-derived HIL measurement |

A green parser/replay test must never be described as `PHYSICALLY VERIFIED` or as a live HIL pass. A missing live marker is `BLOCKED`/`FAIL` according to the runner contract, never an implicit pass.

## Current automated surfaces

Local verification is the default project workflow. GitHub Actions are manual-only and are not required for normal verification. The native host manifests, WebUI typecheck/tests, XR tests, HIL replay/parser contracts, startup/storage/runtime source contracts, production Web build checks, ESP-IDF production firmware build and the isolated nRF wrapper build/test launch may all be run locally. The exact native-suite count is derived from the manifests; do not hard-code an old count in acceptance decisions.

The automatic nRF test path is intentionally narrower than generic PlatformIO test discovery: only suites named `test_sw_*` may run under `nRF54L15/pio.ps1 test` / `Tests: All (no flash)`, and only after those suites are explicitly qualified as hardware-free software/native tests. `--without-uploading` alone is not accepted as proof that an embedded test is hardware-free. Physical nRF target/HIL suites remain separate explicit actions. The current nRF software-test surface is empty and reports `SKIPPED`.

Relevant explicit HIL tools:

- `tools/run_boot_responsiveness_hil.ps1`: startup/recovery/input/priority telemetry. Replay validates the parser; live mode uses COM4 only for programming and the passive UART log port (normally COM6) for evidence.
- `tools/run_storage_spi_hil.ps1`: passive storage/SPI2/recorder latency capture. Replay validates the parser; live mode never flashes firmware and requires a real log port.
- `tools/run_keypad_timing_hil.ps1`: opt-in ADC broker and ESP timer timing profile. Numerical performance acceptance remains dependent on a measured board baseline.
- `tools/run_pipeline_hil.ps1`: broader production measurement pipeline gate, including recorder/API/runtime evidence and explicitly separated manual observations.

## Requirement -> proof mapping

| Requirement | Automated software proof | Required live proof before physical acceptance |
| --- | --- | --- |
| **Keypad end-to-end** | Host keypad decode/ADC ownership tests; host `ui_home_navigation` behavior suite verifies HOME menu mapping, UP/DOWN wrap, RIGHT/OK selection and LEFT-to-HOME behavior; runtime breadcrumbs include `KEYPAD_RAW_TRANSITION`, `KEYPAD_BUTTON_PRESS`, `KEYPAD_EVENT_POST`, `KEYPAD_EVENT_DISPATCH`, and post-handler `UI_KEY_RESULT` state evidence | All five physical keys LEFT/UP/DOWN/RIGHT/OK must each produce the expected runtime chain and the intended visible UI change; no stale delayed navigation burst. `UI_KEY_RESULT` proves software state after the handler, not TFT pixels |
| **SD/SPI2 latency** | `storage_spi_contract.ps1` replay tests validate parsing, exact threshold boundaries, malformed/missing telemetry and error classification | Live `STORAGE_SPI_TIMING`/`RECORDER_IO_TIMING` samples on the real SD/TFT shared SPI2 path; thresholds must come from a recorded real-board baseline, not synthetic replay values |
| **Recorder integrity / dropped frames** | Host recorder/cache/write-state suites validate deterministic write/finalization logic | Live recording must end with normal `STOP_COMPLETE`, `drops=0`, no acquisition read/buffer failure and a readable completed session |
| **Memory floors / PSRAM fallback** | Runtime-observability host policy tests prove PSRAM-preferred allocation, internal fallback, dual-allocation failure handling and deterministic top-N/equality behavior | Real `MEM_RUNTIME` measurements under the intended workload are required for physical memory-floor acceptance and fragmentation/timing claims |
| **Startup responsiveness** | Startup/recovery source contracts and boot-HIL replay parser verify ordering and thresholds | Live `startup_total <= 5000 ms`, bounded recovery windows, responsive physical keypad/TFT, no spontaneous shutdown on the populated card |
| **Power shutdown** | Host power-button policy proves early-boot disarm, explicit post-startup arming and ordered software latch policy | Real post-startup GPIO21 long press must trigger ordered safe shutdown; raw edge-to-latch timing and GPIO18 power-cut behavior remain physical measurements |
| **nRF orientation and logger status** | Future nRF native tests for accelerometer/gravity-vector tilt policy, filtering/hysteresis, BLUE mapping and sticky status state | Remote-profile live proof on plain XIAO nRF54L15 with external GY-BMI160 on local D11/D12 I2C plus external LEDs; main profile separately proves boot/communication without the remote-only IMU/LED assembly |
| **nRF Channel Sounding geometry** | Future nRF scheduling/ranging/matrix tests and protocol/schema contracts | Physical pair/multi-node Channel Sounding, dynamic initiator turns, missing/reconnected nodes and full topology where required |

## Limitations that must stay explicit

Parser tests do not exercise DMA, SPI2 bus arbitration, FreeRTOS scheduling, physical keypad debounce, TFT rendering, SD-card electrical/media behavior, sensor wiring, real PSRAM timing, nRF GPIO/local-GY-BMI160/upstream-I2C behavior or Bluetooth Channel Sounding. A UART marker can prove that firmware reached a software point; it cannot by itself prove that the display changed pixels, a remote LED changed state, a GY-BMI160 was physically read correctly, a sensor was physically vertical or that a physical signal met its electrical timing.

Physical TFT/keypad behavior therefore remains live-board evidence unless a machine-observable sensor/telemetry path independently measures it. Manual confirmation switches are useful for attaching an operator observation to a complete run, but they are not interchangeable with automated physical measurements.

## Recording evidence

For every live performance/HIL result keep the full UART/USB log and record at least: firmware SHA, date, board/variant, toolchain/platform versions, wiring/topology, SD card and ESP settings where applicable, sample rate, active recorder channel configuration, object/session population, nRF node count, Channel Sounding/tilt/logger parameters, runner thresholds, maximum observed values, acquisition-health markers and recorder dropped-frame result. For remote nRF orientation HIL also record the GY-BMI160 module/wiring and local-I2C configuration. If any of these inputs materially changes, the old numerical baseline must not silently become the new acceptance threshold.
