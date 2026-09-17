# Verification evidence matrix

This document is the authoritative boundary between automated software evidence
and physical-board proof. Historical snapshots remain audit history; current
repository state and this matrix are authoritative for acceptance decisions.

## Evidence classes

| Class | What runs | What it proves | What it does **not** prove |
| --- | --- | --- | --- |
| **HOST C** | Native C suites from `tests/host/manifest.json` and `tests/host/manifest.d/*.json` | Pure/stateful C policy, lifecycle/error paths and architecture seams exercised by the suite | ESP32/nRF timing, radio, GPIO/I2C/SPI electrical behavior |
| **PYTHON SOURCE/ARCHITECTURE CONTRACT** | `tests/python` source/config/layout checks | Required architecture/toolchain/configuration remains present and forbidden regressions are absent | Runtime or physical behavior |
| **NRF BUILD** | `nRF54L15/ncs.ps1 build` with pinned NCS v3.4.0 | The canonical common XIAO application configures/links with the authoritative production SDK/board target | Boot, USB, sensors, upstream I2C, radio behavior or Channel Sounding accuracy |
| **NRF PLATFORMIO COMPATIBILITY** | Explicit `nRF54L15: PlatformIO compatibility build` / `pio.ps1 build` | Sources still compile against the historical Seeed/Zephyr 4.2.1 compatibility stack | Production artifact validity or SoftDevice Channel Sounding support |
| **NRF NATIVE/UNIT** | Host `coverage-nrf` today; later reviewed NCS-native `test_sw_*` runner | Deterministic nRF policy/state contracts actually exercised | Physical target/radio/sensor behavior |
| **HIL REPLAY/PARSER** | Saved/synthetic logs through HIL parsers | Parser and threshold/error-classification logic | Current board behavior |
| **LIVE BOARD HIL** | Runner observes telemetry from connected target | Only named machine-observable properties gated by that run | Uninstrumented behavior or unrelated hardware claims |
| **NRF SINGLE-BOARD HIL** | Physical plain XIAO; remote profile adds BMI160/BME280/LEDs | Named boot/I2C/sensor/orientation/GPIO checks | Multi-node ranging or full TCA topology |
| **NRF MULTI-NODE/FULL-TOPOLOGY HIL** | Two+ XIAOs or complete ESP32/TCA topology | Named pairwise CS, role rotation, reconnect/missing-node and topology checks | Any unmeasured property |
| **MANUAL PHYSICAL ATTESTATION** | Operator-supplied explicit observation | Only that named observation | Automated measurement equivalence |

A parser/build/native pass is never `PHYSICALLY VERIFIED`.

## Current automated surfaces

Local verification is the default workflow; GitHub Actions are not required.
The canonical nRF surface is:

```powershell
python tools/run_local_verify.py --scope nrf
```

It runs `coverage-nrf`, the NCS v3.4.0 production build, then an explicit
NCS-native embedded-test boundary. That final embedded stage currently reports
`SKIPPED` because no reviewed NCS-native `test_sw_*` runner exists.

`Tests: All (no flash)` / `tools/run_suite.ps1 -Suite All` uses the same NCS
production build. It never substitutes `pio.ps1` as the production builder and
never flashes hardware.

The historical PlatformIO path is compatibility-only. `pio test
--without-uploading` is not accepted as proof of hardware-free execution by
itself.

## Requirement -> proof mapping

| Requirement | Automated software proof | Required live proof before physical acceptance |
| --- | --- | --- |
| Keypad end-to-end | host decode/ADC/UI policy + source contracts | all five keys produce intended runtime chain and visible change |
| SD/SPI2 latency | parser/replay and source contracts | live `STORAGE_SPI_TIMING` / `RECORDER_IO_TIMING` against measured baseline |
| Recorder integrity | host recorder/write/finalization suites | live normal stop, zero drops, readable completed session |
| Memory floors | allocation/runtime-observability host policy | real `MEM_RUNTIME` under intended workload |
| Startup responsiveness | startup/recovery contracts + replay parser | live startup <= approved limit and responsive TFT/keypad |
| Power shutdown | host latch/order policy | real GPIO21 hold -> ordered shutdown and physical power-cut behavior |
| nRF common firmware/toolchain | Python contracts + `coverage-nrf` + canonical NCS v3.4.0 link | one-board boot/flash evidence when physical acceptance is required |
| nRF orientation/snapshot/logger | deterministic driver/runtime/orientation tests | remote plain XIAO + BMI160/BME280/LED wiring and behavior |
| nRF Channel Sounding scheduler | `nrf_cs_scheduler` host tests | none for pure scheduler itself |
| nRF Channel Sounding execution/geometry | future MAX-51 adapter mocks + NCS target build | real pair/multi-node CS, repeatability, initiator rotation and RF-environment behavior |

## Canonical nRF toolchain evidence

The production build must come from:

- NCS v3.4.0;
- upstream `xiao_nrf54l15/nrf54l15/cpuapp`;
- `nRF54L15/zephyr/prj.conf` for the current production configuration;
- `nRF54L15/ncs.ps1 build` as the supported Windows entry point.

The MAX-45 `ncs/channel_sounding.conf` is currently a proof overlay invoked by
`ncs.ps1 cs-proof`; it is not silently part of production until MAX-51 promotes
reviewed RAS/CS configuration.

`ncs.ps1 flash` is an explicit physical write and therefore cannot appear in a
no-flash automated suite. `ncs.ps1 monitor -Port ...` merely opens logs and is
not HIL acceptance.

## nRF hardware invariants

The target is the plain XIAO nRF54L15. Remote nodes use GY-BMI160+BME280 on
private D11/P0.03 SCL + D12/P0.04 SDA (`i2c30` TWIM) and XIAO `3V3_OUT`.
Upstream D4/P1.10 SDA + D5/P1.11 SCL uses `i2c22` TWIS toward ESP32/TCA9548A.
RJ45 pin 2 +3.3 V is RESERVED/NC.

Wire compatibility stays:

- 12-byte `SENSOR_REFRESH`;
- 102-byte REMOTE snapshot;
- first 28 bytes compatible with legacy STATUS.

Successful target linkage does not prove any of this wiring physically.

## Explicit HIL tools

Existing ESP32-side explicit HIL tools remain separate, including:

- `tools/run_boot_responsiveness_hil.ps1`;
- `tools/run_storage_spi_hil.ps1`;
- `tools/run_keypad_timing_hil.ps1`;
- `tools/run_pipeline_hil.ps1`.

Future nRF one-board/multi-node runners must preserve the same evidence
separation.

## Recording live evidence

For every live result record at least firmware SHA, date, board/variant,
toolchain, wiring/topology, relevant parameters/thresholds and full logs. For
remote nRF HIL also record the BMI160/BME280 module/wiring, `3V3_OUT` rail and
RJ45 pin-2 NC check. For Channel Sounding record node count, roles, antenna
configuration, RF environment and all range/quality parameters used by the
acceptance gate.
