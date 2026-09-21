# Task: develop and run nRF54L15 tests

Status: **runtime sensor/topology foundation and platform-neutral Channel
Sounding scheduler implemented; real NCS Bluetooth/RAS pair execution and
physical HIL remain future slices**.

Sources of truth:

- [`../ARCHITECTURE.md`](../ARCHITECTURE.md) — physical topology and architecture;
- [`local-verification.md`](local-verification.md) — current local build/test entry points;
- [`verification-evidence-matrix.md`](verification-evidence-matrix.md) — evidence classes;
- [`../nRF54L15/README.md`](../nRF54L15/README.md) — nRF production/toolchain contract.

## 1. Production toolchain and one-image rule

Every installed XIAO nRF54L15 uses one common source tree and one runtime
firmware image. MAIN/REMOTE wired role and Channel Sounding
initiator/reflector role are runtime state, never separate firmware variants.

The canonical production toolchain is:

- **nRF Connect SDK v3.4.0**;
- Nordic Zephyr 4.4.0 from that SDK;
- upstream board `xiao_nrf54l15/nrf54l15/cpuapp`;
- Nordic SoftDevice Controller for Channel Sounding-capable future production
  configuration.

Canonical Windows commands:

```powershell
.\nRF54L15\ncs.ps1 build
.\nRF54L15\ncs.ps1 clean
.\nRF54L15\ncs.ps1 flash
.\nRF54L15\ncs.ps1 monitor -Port COM7
```

`ncs.ps1` self-launches the pinned Nordic sdk-manager toolchain if `west` is
not already available and uses a no-space `SUBST` source alias when required.
Build output lives under `%USERPROFILE%\.ncs-workspaces\esp32_geophone_source_locator`.

The historical PlatformIO baseline remains commit-pinned to Seeed platform
revision `1ec1287f8e4bc4067a6fd593991e36875aef989f`, PlatformIO Core 6.1.19 and
Zephyr 4.2.1. It is now **compatibility smoke only**. The board environment
remains pinned, while a separate `[env:native]` is reserved for qualified
`test_sw_*` software tests. Neither environment nor `pio.ps1 upload` defines the
official production artifact.

The MAX-45 additive Channel Sounding proof remains reproducible through:

```powershell
.\nRF54L15\ncs.ps1 cs-proof
.\nRF54L15\ncs.ps1 compare
.\nRF54L15\ncs.ps1 probe
```

`ncs/channel_sounding.conf` is a proof fixture, not a second production
configuration. MAX-51 will promote the reviewed permanent RAS/CS options only
when the real adapter exists.

## 2. Hardware/topology contract

The project uses the **plain Seeed Studio XIAO nRF54L15**, not the Sense.
There is one main-enclosure XIAO and up to four remote XIAOs behind TCA9548A.
ESP32-S3 is the upstream I2C controller.

Remote private sensor wiring:

| XIAO pin | nRF54L15 | Use |
| --- | --- | --- |
| D4/SDA-0 | P1.10 | upstream/TCA SDA, `i2c22` TWIS |
| D5/SCL-0 | P1.11 | upstream/TCA SCL, `i2c22` TWIS |
| D11/SCL-1 | P0.03 | private sensor SCL, `i2c30` TWIM |
| D12/SDA-1 | P0.04 | private sensor SDA, `i2c30` TWIM |

Remote GY-BMI160 and BME280 are powered from XIAO `3V3_OUT`. RJ45 pin 2
(+3.3 V) is RESERVED/NC and must not be paralleled with `3V3_OUT`.

The main node does not require the remote-only sensors or four external LEDs.
Both sensors are optional runtime peripherals in the common firmware; their
absence on MAIN is not a boot error.

Existing upstream wire compatibility is frozen:

- discovery/assignment/status contract remains as documented;
- `SENSOR_REFRESH` request = 12 bytes;
- REMOTE extended snapshot = 112 bytes;
- first 28 snapshot bytes remain legacy STATUS compatible;
- nRF uptime and ESP32 receipt uptime are separate clock domains, not TDOA
  synchronization.

## 3. Current software architecture

Implemented and host-tested pieces include:

- `nrf_node_state` and bootstrap role assignment;
- upstream TWIS target/runtime;
- private sensor-bus abstraction and Zephyr bridge;
- BMI160/BME280 drivers and REMOTE snapshot aggregation;
- integer orientation estimator / motion gate;
- ESP32 topology and sensor consumer;
- platform-neutral `nrf_cs_scheduler` with at most five nodes / ten undirected
  pairs, one active directed attempt, retries, stale-token rejection,
  disconnect/rejoin and explicit missing/quality state.

The platform-neutral scheduler deliberately owns no Bluetooth connection.
MAX-51 is the reviewed NCS/RAS directed-pair adapter slice. Multi-node execution
authority/handoff is a separate control-plane problem because `BT_MAX_CONN=1`
does not allow one nRF to retain simultaneous control links to all peers.

## 4. Channel Sounding test contract

The target technology is Bluetooth 6 Channel Sounding, not RSSI-only ranging.
The wired MAIN node is not a permanent BLE/CS master. A complete available-node
cycle must eventually allow every connected XIAO to receive initiator turns.

Software tests must cover:

- deterministic C(n,2) pair enumeration for 0..5 nodes;
- directed initiator fairness independent of wired role;
- one active attempt at a time;
- cycle/membership/attempt token rejection of stale results;
- timeout/retry/exhaustion/outlier bookkeeping;
- disconnect/rejoin and partial node sets;
- explicit missing/invalid matrix cells;
- later NCS adapter phase transitions, disconnect cleanup and RAS data paths.

Target builds and mocks do **not** prove ranging accuracy. Real pairwise
repeatability/accuracy, antenna/RF effects and geometry quality remain HIL.

## 5. Orientation and remote LEDs

Remote LEDs:

| nRF54L15 pin | XIAO | LED | Planned meaning |
| --- | --- | --- | --- |
| P1.04 | D0 | BLUE | verticality/tilt |
| P1.05 | D1 | GREEN | normal logger state |
| P1.06 | D2 | YELLOW | latched warning |
| P1.07 | D3 | RED | latched error |

The main node has none of these external LEDs. BLUE is independent of the
logger traffic-light latches. Physical polarity, resistor wiring and the
installed BMI160 reference axis must be HIL-verified before the backend is
claimed complete.

## 6. Test levels

### A. Build/configuration

Required automated evidence:

1. canonical `ncs.ps1 build` configures and links the full common application
   with NCS v3.4.0;
2. source and generated output remain isolated from ESP-IDF root build;
3. no role-specific production images exist;
4. upstream TWIS/private TWIM overlay stays intact;
5. 12-byte/112-byte wire constants stay intact;
6. `Run All` and `--scope nrf` use NCS, not PlatformIO;
7. PlatformIO is named compatibility-only everywhere it remains exposed.

### B. Software unit/native

Pure policy/state logic belongs in host/native tests. Existing `coverage-nrf`
is part of the canonical nRF verification surface.

The NCS-native test application is now the canonical embedded bridge/runtime
surface. It uses Zephyr `ztest` and the production sensor bridge sources. The
regular profile checks the real Zephyr I2C API path against a deterministic fake
bus, BMI160/BME280 bridge argument guards and remote/main role behavior:

```powershell
.\nRF54L15\ncs.ps1 ncs-test-build
```

The Channel Sounding profile merges the production `zephyr/prj.conf` with the
test application configuration, so the real RAS/CS-DE runtime branch is
compiled together with the bridge tests:

```powershell
.\nRF54L15\ncs.ps1 ncs-cs-test-build
```

These two commands are target compile evidence (`COMPILES`), not a claim that
the image was flashed or that the ztests ran on a physical XIAO. The optional
`ncs.ps1 native-test` action can execute the same app under `native_sim` when a
host GCC is installed. The PlatformIO `test_sw_upstream_protocol` suite remains
compatibility-only; its no-upload mode does not replace NCS evidence.

### C. One-board HIL

Common checks:

- canonical NCS flash, boot and USB/serial logging;
- identity/reset behavior;
- upstream I2C target behavior;
- later Bluetooth discovery/connection lifecycle.

Remote profile additionally checks external LEDs, GY-BMI160+BME280 on D11/D12,
`3V3_OUT` power, valid samples, snapshot freshness, static/tilted/moving
orientation and independent sensor-failure handling.

MAIN profile proves boot/communication without remote-only sensors and LEDs.

### D. Multi-node HIL

With two or more XIAOs:

- real Channel Sounding pair execution;
- repeated pairwise ranges and quality;
- initiator/reflector rotation;
- disconnect/reconnect/reboot;
- missing-node behavior;
- eventual matrix completion/recovery.

The repeatability/accuracy gate has a separate runner and must not be inferred
from a successful build. The runner accepts either a captured serial log or a
live port and evaluates every completed pair sample against the measured
fixture distance, success rate, bias, p95 error, maximum error and repeatability
standard deviation. A live PASS additionally requires explicit fixture
confirmation:

```powershell
# Replay a previously captured run without touching hardware.
.\tools\run_nrf_cs_accuracy_hil.ps1 `
  -LogPath artifacts\nrf-cs-accuracy-hil\capture.log `
  -ExpectedDistanceMm 1000 -MaxErrorMm 50 -Repeats 20 -MinSuccessRate 0.90

# Live two-board run; only this explicit command opens the ESP32 serial port.
.\tools\run_nrf_cs_accuracy_hil.ps1 `
  -Port COM4 -ExpectedDistanceMm 1000 -MaxErrorMm 50 `
  -Repeats 20 -MinSuccessRate 0.90 -FixtureConfirmed
```

The physical fixture must contain the ESP32/TCA topology controller and two
XIAO nRF54L15 nodes at a measured separation. The runner does not erase flash,
erase NVS or write eFuses. Without two assigned nodes, a complete capture, or
fixture confirmation, the result is `BLOCKED`, never physical proof.

### E. Full ESP32 + TCA topology HIL

Validate MAIN always-visible behavior, one-hot TCA remote selection, private
remote sensor buses, upstream snapshots and complete orchestration without
breaking seismic acquisition.

After any REMOTE snapshot-size change, this HIL gate must be repeated with the
current production image: verify the ESP32/TCA path clocks exactly 112 bytes,
the 28-byte STATUS prefix remains valid, the full-frame CRC covers bytes 0..109,
and the next transaction is clean after STOP. The current 112-byte gate is not
considered physically verified in this checkout until those observations are
recorded with the XIAO/TCA setup and, where applicable, the Channel Sounding
peer set.

## 7. Evidence vocabulary

- `COMPILES`: canonical target build/link succeeded;
- `HOST TESTED` / `NATIVE TESTED`: deterministic software checks passed;
- `TARGET TESTED`: named behavior ran on one physical target;
- `HIL TESTED`: named physical scenario passed;
- `PHYSICALLY VERIFIED`: named manual/electrical observation was confirmed.

Never promote build/replay/native evidence into physical proof.

## 8. Regression contract

Future nRF changes must preserve:

- canonical NCS production build;
- nRF host contracts;
- ESP32 build/tests;
- WebUI tests/build/typecheck;
- protocol/schema contracts;
- one-image/no-role-variant invariant;
- explicit physical HIL separation;
- no GitHub Actions dependency.

The old PlatformIO compatibility build may be run diagnostically, but failure or
success there does not replace the canonical NCS production result unless a
future dependency decision explicitly changes this contract.

## 9. Remaining gates

Open work includes:

1. MAX-51 NCS RREQ/RRSP directed-pair adapter;
2. multi-node execution authority / initiator handoff;
3. physical Channel Sounding accuracy/repeatability HIL;
4. external LED backend and final latch UX;
5. installed BMI160 axis/polarity and remote electrical verification;
6. full topology HIL and any later ESP32 matrix/geometry protocol.
