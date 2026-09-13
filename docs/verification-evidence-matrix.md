# Verification evidence matrix

This document is the authoritative boundary between automated software evidence and physical board proof. Historical counts and snapshots in `docs/firmware-test-plan.md` remain audit history; when they conflict with this file or current CI, use this file plus the current workflow as the source of truth.

## Evidence classes

| Class | What runs | What it proves | What it does **not** prove |
| --- | --- | --- | --- |
| **HOST C** | Native C suites from `tests/host/manifest.json` and `tests/host/manifest.d/*.json`, compiled against deterministic ESP-IDF/FreeRTOS/component fakes | Pure/stateful C behavior, error paths, lifecycle policy, queue/lock ownership contracts that the suite actually exercises | ESP32 timing, DMA, dual-core scheduling, physical GPIO/I2C/SPI, SD electrical behavior, TFT pixels |
| **PYTHON SOURCE/ARCHITECTURE CONTRACT** | `tests/python` checks that inspect source/configuration/layout or deterministic text artifacts | Required architecture/configuration remains present; forbidden regressions are absent | Runtime behavior, FreeRTOS timing, peripheral behavior, physical device state |
| **HIL REPLAY/PARSER** | PowerShell HIL runner invoked with `-LogPath`; Python tests replay saved/synthetic UART text through the same parser | Parser correctness, threshold boundaries, malformed/missing telemetry classification, failure-marker classification | Current board behavior. Replay never proves DMA, SPI2 arbitration, scheduling, keypad debounce, TFT rendering, SD media/electrical behavior, real PSRAM timing or current wiring |
| **LIVE BOARD HIL** | Runner opens the actual passive UART log port and observes telemetry produced by the connected target during the requested scenario | Only the machine-observable properties explicitly gated by that runner on that physical run | Uninstrumented physical behavior; visual/manual observations unless independently measured and encoded in telemetry |
| **MANUAL PHYSICAL ATTESTATION** | Operator observes a physical effect and supplies an explicit confirmation switch where a runner supports it | The operator states that one named physical observation was seen in that run | It is not an automated measurement and must never be reported as equivalent to a telemetry-derived HIL measurement |

A green parser/replay test must never be described as `PHYSICALLY VERIFIED` or as a live HIL pass. A missing live marker is `BLOCKED`/`FAIL` according to the runner contract, never an implicit pass.

## Current automated surfaces

The main CI workflow runs the native host manifests, WebUI typecheck/tests, XR tests, HIL replay/parser contracts, startup/storage/runtime source contracts, production Web build checks and an ESP-IDF 5.5.4 production firmware build. Native suites are discovered from `tests/host/manifest.json` plus `tests/host/manifest.d/*.json`; do not hard-code an old suite count in acceptance decisions.

Relevant explicit HIL tools:

- `tools/run_boot_responsiveness_hil.ps1`: startup/recovery/input/priority telemetry. Replay validates the parser; live mode uses COM4 only for programming and the passive UART log port (normally COM6) for evidence.
- `tools/run_storage_spi_hil.ps1`: passive storage/SPI2/recorder latency capture. Replay validates the parser; live mode never flashes firmware and requires a real log port.
- `tools/run_keypad_timing_hil.ps1`: opt-in ADC broker and ESP timer timing profile. Numerical performance acceptance remains dependent on a measured board baseline.
- `tools/run_pipeline_hil.ps1`: broader production measurement pipeline gate, including recorder/API/runtime evidence and explicitly separated manual observations.

## Coverage-backlog behavioral host suites

The `manifest.d` suites close the audited low/zero-coverage lifecycle debt with executable C behavior rather than grep-only checks:

| Suite | What it proves on host | Physical/runtime boundary |
| --- | --- | --- |
| `sensor_bus_lifecycle` | I2C bus/device creation failure handling, cleanup, idempotent init, handle validation and shutdown state | Does not prove electrical I2C signaling or real sensor presence |
| `status_indicator_state` | Event subscription lifecycle and NORMAL/WARNING/ERROR/sticky state transitions | Does not prove LED electrical output or visible color on hardware |
| `device_status_aggregation` | Input validation and deterministic aggregation/snapshot behavior, including invalid source/index cases | Does not prove real sensor readings or concurrent ESP32 producer timing |
| `storage_service_lifecycle` | Queue/semaphore/task allocation rollback, atomic busy gate, queue-send failure recovery, callback ordering and shutdown/re-init cleanup | Does not prove FreeRTOS scheduler timing or physical SD latency |
| `object_service_lifecycle` | Queue/task rollback, busy gate, command validation, worker processing, callback/observer lifecycle and shutdown cleanup | Does not prove FATFS media timing or archive-scale performance |
| `insights_bridge_lifecycle` | Non-fatal initialization failure, retry worker/handler failure policy, event-to-worker notification, retry-to-success and idempotent success | Does not prove cloud/network reachability, credentials or live telemetry delivery |

These suites use the same `tools/run_host_tests.py` compiler path and `-Wall -Wextra -Werror` policy as the original host manifest. Their PASS status is HOST C evidence only.

## Requirement -> proof mapping

| Requirement | Automated software proof | Required live proof before physical acceptance |
| --- | --- | --- |
| **Keypad end-to-end** | Host keypad decode/ADC ownership tests; source contracts; boot HIL replay verifies `KEYPAD_RAW_TRANSITION`, Button/app-event/UI parser semantics where present | All five physical keys LEFT/UP/DOWN/RIGHT/OK must each produce a first-hop decode/press trace, `KEYPAD_EVENT_POST`, `KEYPAD_EVENT_DISPATCH`, and the intended visible UI change; no stale delayed navigation burst |
| **SD/SPI2 latency** | `storage_spi_contract.ps1` replay tests validate parsing, exact threshold boundaries, malformed/missing telemetry and error classification | Live `STORAGE_SPI_TIMING`/`RECORDER_IO_TIMING` samples on the real SD/TFT shared SPI2 path; thresholds must come from a recorded real-board baseline, not synthetic replay values |
| **Recorder integrity / dropped frames** | Host recorder/cache/write-state suites validate deterministic write/finalization logic | Live recording must end with normal `STOP_COMPLETE`, `drops=0`, no acquisition read/buffer failure and a readable completed session |
| **Memory floors / PSRAM fallback** | Runtime-observability host policy tests prove PSRAM-preferred allocation, internal fallback, dual-allocation failure handling and deterministic top-N/equality behavior | Real `MEM_RUNTIME` measurements under the intended workload are required for physical memory-floor acceptance and fragmentation/timing claims |
| **Startup responsiveness** | Startup/recovery source contracts and boot-HIL replay parser verify ordering and thresholds | Live `startup_total <= 5000 ms`, bounded recovery windows, responsive physical keypad/TFT, no spontaneous shutdown on the populated card |
| **Power shutdown** | Host power-button policy proves early-boot disarm, explicit post-startup arming and ordered software latch policy | Real post-startup GPIO21 long press must trigger ordered safe shutdown; raw edge-to-latch timing and GPIO18 power-cut behavior remain physical measurements |

## Limitations that must stay explicit

Parser tests do not exercise DMA, SPI2 bus arbitration, FreeRTOS scheduling, physical keypad debounce, TFT rendering, SD-card electrical/media behavior, sensor wiring, or real PSRAM timing. A UART marker can prove that firmware reached a software point; it cannot by itself prove that the display changed pixels or that a physical signal met its electrical timing.

Physical TFT/keypad behavior therefore remains live-board evidence unless a machine-observable sensor/telemetry path independently measures it. Manual confirmation switches are useful for attaching an operator observation to a complete run, but they are not interchangeable with automated physical measurements.

## Recording evidence

For every live performance/HIL result keep the full UART log and record at least: firmware SHA, date, board, SD card, SPI/SDSPI frequency, sample rate, active recorder channel configuration, object/session population, runner thresholds, maximum observed values, acquisition-health markers and recorder dropped-frame result. If any of these inputs materially changes, the old numerical baseline must not silently become the new acceptance threshold.
