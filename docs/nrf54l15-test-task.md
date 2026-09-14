# Task: develop and run nRF54L15 tests

Status: **foundation implemented; protocol/radio work still planned**. The
upstream Zephyr TWIS platform adapter and its host/configuration contracts now
exist, while BLE, Bluetooth Channel Sounding, production role-assignment/wire
framing, remote LED policy and the remaining physical HIL work stay future
slices.

Related sources of truth:

- repository boundaries and physical topology: [`../ARCHITECTURE.md`](../ARCHITECTURE.md);
- master local validation flow: [`local-verification.md`](local-verification.md);
- evidence vocabulary: [`verification-evidence-matrix.md`](verification-evidence-matrix.md);
- existing ESP32 test layers: [`firmware-test-plan.md`](firmware-test-plan.md);
- nRF project entry point: [`../nRF54L15/README.md`](../nRF54L15/README.md).

## 1. Goal and non-goals

The future implementation/test work must make every installed XIAO nRF54L15
usable from the same production firmware image. It must eventually provide the
ESP32-S3 with node identity/topology, Bluetooth 6 Channel Sounding range data,
and remote-geophone orientation status while keeping `nRF54L15/` isolated from
the ESP-IDF root project.

This task does not authorize:

- separate `central` and `remote` production firmware variants;
- separate production PlatformIO environments for node roles;
- an invented I2C address-allocation scheme before it is reviewed;
- hard-coded tilt/blink thresholds that have not been approved/measured;
- treating software/native tests as physical HIL evidence;
- changing ESP32/WebUI production behavior merely to make nRF tests easier.

## 2. Current verified baseline

The repository contains the independent nRF bring-up project:

```text
nRF54L15/
  platformio.ini
  pio.ps1
  include/
  src/
  test/
  zephyr/app.overlay
  zephyr/prj.conf
```

The current production environment is:

```ini
[env:seeed-xiao-nrf54l15]
platform = https://github.com/Seeed-Studio/platform-seeedboards.git#1ec1287f8e4bc4067a6fd593991e36875aef989f
framework = zephyr
board = seeed-xiao-nrf54l15
```

The current `src/main.c` is only a temporary board/toolchain blink through the
onboard `led0` alias. It is not the future external remote LED implementation
and proves no Channel Sounding, topology or IMU functionality. The new
`nrf_upstream_target_zephyr` module is deliberately not wired into `main.c` in
its foundation slice.

The supported Windows entry point is `nRF54L15/pio.ps1`. The checkout path
contains spaces, so the wrapper exposes the project through a temporary
no-space `SUBST` drive and places generated PlatformIO/Zephyr data under:

```text
%USERPROFILE%\.platformio-workspaces\nrf54l15
```

Supported commands are:

```powershell
.\nRF54L15\pio.ps1 build
.\nRF54L15\pio.ps1 clean
.\nRF54L15\pio.ps1 upload
.\nRF54L15\pio.ps1 monitor
.\nRF54L15\pio.ps1 test
```

A direct `pio run -d ".\nRF54L15"` command is not the normal workflow.

The baseline wrapper build was verified on 2026-09-14 with PlatformIO Core
6.1.19, SeeedStudio platform 1.0.0 at
`1ec1287f8e4bc4067a6fd593991e36875aef989f`, framework package
`3.40201.251021`, and Zephyr 4.2.1. The production PlatformIO environment is
commit-pinned to that verified Seeed platform revision. A future platform or
framework refresh must be an explicit dependency update followed by the same
build/software/HIL qualification gates rather than an implicit moving-branch
change.

## 3. Production architecture contract

### One production firmware for every node

All installed XIAO nRF54L15 boards use:

- one production source tree;
- one production firmware implementation;
- one production firmware binary/configuration shape;
- runtime node identity/topology/ranging roles.

There must be no compile-time `central`/`remote` firmware split. A future
host/native-only test environment is allowed if required for software testing;
it must never become a second production node-role image.

### Wired topology

The planned topology is:

```text
ESP32-S3
|
+-- shared upstream I2C
|     |
|     +-- main-enclosure nRF54L15 (always visible)
|     |
|     +-- TCA9548A
|           |
|           +-- selected channel -> remote nRF54L15
|           |                         +-- local I2C -> GY-BMI160 + BME280
|           +-- another channel  -> remote nRF54L15
|                                     +-- local I2C -> GY-BMI160 + BME280
|           +-- ...
|
+-- existing diploma hardware
```

There is one main-enclosure nRF54L15 and one remote nRF54L15 per connected
remote geophone. Four remote geophones therefore mean five nRF54L15 boards.

The intended discovery concept is:

1. disable all TCA9548A downstream channels;
2. identify the nRF target still visible upstream as the main-enclosure node;
3. select TCA channels one at a time;
4. associate the newly visible remote target with that channel.

This remains a topology concept, not an implemented production protocol.
Multiple visible nRF nodes may otherwise share the same target address. The
TWIS platform foundation intentionally accepts an address from its caller and
does not invent `DISCOVERY`, MAIN or REMOTE numeric addresses. Address
assignment, identity provisioning, one-hot mux enforcement and collision
recovery must be frozen before production role-assignment/wire framing is
wired into runtime behavior.

ESP32-S3 remains the upstream wired-I2C controller. Each remote nRF54L15 is
also the controller of its private second I2C bus, where its local GY-BMI160
and BME280 are connected. Neither sensor is directly visible through the
ESP32/TCA9548A bus; the future upstream node snapshot carries aggregated
sensor data. Both local sensors are powered from the XIAO `3V3_OUT` rail, not
from an nRF GPIO. RJ45 pin 2 (+3.3 V) is `RESERVED/NC` and must not be tied to
`3V3_OUT`.

### Bluetooth 6 Channel Sounding

The target ranging technology is Bluetooth 6 Channel Sounding, not an RSSI-only
replacement when Channel Sounding is available.

The main-enclosure nRF54L15 is not a permanently privileged Bluetooth master.
During a complete measurement cycle every connected nRF54L15 must receive an
initiator turn. Future implementation/tests must cover:

- Channel Sounding session scheduling;
- runtime initiator/reflector role switching;
- pairwise ranging;
- repeated-measurement aggregation;
- quality/confidence;
- timeout/missing/outlier handling;
- distance-matrix construction for currently available nodes;
- reconnect/reboot/partial-topology behavior;
- node count independent of the reference four-remote layout.

The exact geometry solver and acceptance thresholds remain separate design
work.

## 4. Actual remote sensor hardware: plain XIAO + local GY-BMI160/BME280

The installed target is the **plain Seeed Studio XIAO nRF54L15, not the XIAO
nRF54L15 Sense**. The plain board has no built-in IMU. Therefore tests and
future firmware must not rely on the Sense-only `LSM6DS3TR-C`, `imu0`, or
Sense built-in-sensor examples as if that device existed on the installed
hardware.

Each remote geophone node uses an external **GY-BMI160** 6-axis IMU controlled
by its local nRF54L15. The main-enclosure nRF54L15 does not have a geophone
whose verticality must be measured and therefore does not require this
external IMU.

### Local sensor bus allocation and wiring

The XIAO exposes two I2C pin pairs. The project reserves them as follows:

| XIAO pin | nRF54L15 pin | Project use | Connected hardware |
| --- | --- | --- | --- |
| `D4 / SDA-0` | `P1.10` | shared upstream/TCA I2C SDA; `i2c22` target/TWIS foundation | ESP32-S3/TCA9548A |
| `D5 / SCL-0` | `P1.11` | shared upstream/TCA I2C SCL; `i2c22` target/TWIS foundation | ESP32-S3/TCA9548A |
| `D11 / SCL-1` | `P0.03` | private local I2C SCL; `i2c30` controller/TWIM | GY-BMI160 + BME280 |
| `D12 / SDA-1` | `P0.04` | private local I2C SDA; `i2c30` controller/TWIM | GY-BMI160 + BME280 |

The accepted remote power/wiring plan is:

```text
RJ45 +5V        -> XIAO 5V/VBUS
RJ45 GND        -> XIAO GND
RJ45 pin 2 +3V3 -> RESERVED/NC

XIAO 3V3_OUT    -> GY-BMI160 3V3
XIAO 3V3_OUT    -> BME280 3.3V/VCC
XIAO GND        -> both sensor grounds

D12/P0.04 SDA-1 -> GY-BMI160 SDA + BME280 SDA
D11/P0.03 SCL-1 -> GY-BMI160 SCL + BME280 SCL
```

For the selected GY-BMI160 breakout, the assembly proposal is `VIN` NC,
`CS/CSB` to 3.3 V for I2C, `SA0/SDO` to GND for BMI160 address `0x68`, and
`INT1`, `INT2`, `SCX`, `SDX` and `OCS` NC for now. These labels and the
breakout's pull-ups must be verified on the physical module. The BME280 uses
the same local bus; at chip level its CSB must select I2C and SDO must be
defined. Its address is still TBD (`0x76`/`0x77` from its SDO strap), as are
module pull-ups and conversion/history policy. If history is required, future
firmware must retain it in nRF RAM.

The local bus is intentionally private to the remote node. The implementation
uses distinct instances: local D11/D12 remains `i2c30` in controller/TWIM mode,
while the project overlay changes upstream D4/D5 `i2c22` to
`nordic,nrf-twis` with TWIS pin selections on P1.10/P1.11. In the pinned
Zephyr 4.2.1 baseline, `drivers/i2c/i2c_nrfx_twis.c` exposes the standard
Zephyr `i2c_target_register()` / `i2c_target_unregister()` API in buffer mode;
there is no project-specific HAL bypass in this foundation. The
`nrf_upstream_target_zephyr` adapter supplies lifecycle, re-address rollback
and raw `hwinfo_get_device_id()` access. Numeric project addresses and
production command framing remain separate design gates.

Zephyr provides `bosch,bmi160` bindings for both I2C and SPI and a BME280 I2C
binding/Sensor API path. Before production implementation, verify against the
actual GY-BMI160 module:

- module I2C address/selection strap;
- whether SDA/SCL pull-ups are already populated on the module;
- required supply pin and 3.3 V behavior;
- whether INT1/INT2 are needed; if not needed initially, do not consume extra
  XIAO pins merely because the module exposes them;
- driver/Kconfig behavior in the exact Seeed/Zephyr package used by this
  project.

For stationary verticality the accelerometer gravity vector is the primary
absolute reference. Gyroscope data is useful for motion detection and optional
sensor fusion, but gyro integration alone is not a stable static tilt
reference.

The project's mechanical reference remains approximately 10 degrees maximum
tilt. That is not yet a single hard-coded firmware threshold. Filtering,
calibration, threshold, warning band, hysteresis/debounce and invalid-sensor
behavior must be explicit parameters/requirements before implementation.

## 5. Remote LEDs

Each remote node has four external LEDs:

| nRF54L15 pin | XIAO pin | LED | Planned meaning |
| --- | --- | --- | --- |
| `P1.04` | `D0` | BLUE | verticality/tilt indication |
| `P1.05` | `D1` | GREEN | normal logger state |
| `P1.06` | `D2` | YELLOW | latched warning |
| `P1.07` | `D3` | RED | latched error |

The main-enclosure nRF54L15 has none of these four external LEDs. Common
firmware must boot and communicate when both the external LED assembly and the
GY-BMI160 are absent on the main node.

BLUE is independent of the logger traffic-light state. Intended UX:

- accepted vertical state -> BLUE steady;
- tilt outside the accepted state -> BLUE blinks;
- larger filtered tilt error -> faster blink;
- maximum blink rate, thresholds, hysteresis and IMU-failure indication are
  future measured/configured parameters, not invented constants.

Logger LEDs follow the user-facing semantics of the ESP32 status state:

- normal -> GREEN steady;
- WARNING -> YELLOW latched until reset/power cycle;
- ERROR/FATAL -> RED latched until reset/power cycle;
- a later success/normal event does not clear a warning/error latch.

The existing ESP32 effective state uses `ERROR > WARNING > NORMAL`, but the
remote has independent yellow/red outputs. Red-only priority versus simultaneous
yellow+red when both latches are set remains an explicit UX decision that must
be confirmed before implementation/tests freeze the expected physical output.

## 6. Test levels

### A. Build/configuration tests

Prove at minimum:

1. `nRF54L15/` remains an independent PlatformIO project;
2. repository root remains ESP-IDF, not PlatformIO;
3. there is one production nRF firmware/environment and no role-specific
   central/remote production variants;
4. `.\nRF54L15\pio.ps1 clean` then `build` succeeds from the real checkout path
   containing spaces;
5. an unchanged second build succeeds incrementally;
6. generated PlatformIO/Zephyr data stays outside the repository;
7. nRF build is independent from ESP-IDF and WebUI build;
8. toolchain/platform/framework versions are visible so drift can be detected;
9. the pinned XIAO overlay keeps `i2c22` as TWIS on P1.10/P1.11 and leaves
   private `i2c30` in controller/TWIM mode.

### B. Software unit/native tests

Pure policy/state logic should be deterministic and hardware-independent where
possible. Cover:

| Area | Required cases |
| --- | --- |
| Node identity/state | boot, unassigned, main, remote, unavailable, reconnect, reset, invalid transition |
| Runtime topology | main visible with mux closed; selected remote appears; disappearing/reappearing channel; 0..N remotes; duplicate/ambiguous identity rejected |
| Upstream TWIS foundation | callback/device validation; 7-bit caller-supplied address; idempotent register/unregister; successful re-address; unregister failure; failed new-address rollback; failed rollback -> explicit unregistered state; hardware-ID success/error |
| Logger latches | normal, warning latch, error latch, later success does not clear, approved simultaneous warning/error UX |
| Tilt/BLUE policy | vertical, threshold boundaries, monotonic blink-rate mapping, maximum rate, invalid IMU, no chatter near boundary |
| Filtering/hysteresis | startup fill, noisy samples, stationary gravity, motion entry/exit, timeout/reset |
| BMI160 policy | sample-valid/invalid/timeout state handling without pretending host tests prove physical I2C |
| BME280/aggregator policy | local-device discovery, valid/invalid conversion, independent sensor failure, snapshot freshness, optional nRF RAM history and no direct upstream BME target assumption |
| Local sensor bus | separate TWIM/TWIS instance policy, address/strap validation, pull-up conflict policy and malformed/missing sensor responses |
| Ranging validation | valid range, impossible/negative value, stale sample, low confidence, timeout, outlier |
| Pairwise matrix | normalized node keys, update/stale/missing edge, partial matrix |
| CS scheduler | every available node gets initiator turn; reflector pairing; timeout/retry; join/leave; deterministic schedule independent of four-remotes assumption |
| Error handling | malformed transport payload, reset during session, partial recovery without clearing sticky status |

Native tests do not prove GPIO, the physical GY-BMI160/BME280, I2C electrical
behavior or radio ranging.

### C. Automatic `Tests: All (no flash)` boundary

The project-wide automatic test path must remain hardware-free.

Important PlatformIO rule: `pio test --without-uploading` prevents firmware
upload, but an embedded test can still require a physical test/serial port.
Therefore the automatic nRF `test` action must execute only test suites that
have been explicitly qualified as software/native tests. Physical target/HIL
suites must not become part of `Tests: All (no flash)` just because a new
`test_*` directory appears.

A future host/native-only PlatformIO environment is permitted if that is the
cleanest way to execute software tests. Such an environment is a test harness,
not another production firmware variant.

### D. One-board HIL

For a physical plain XIAO nRF54L15, distinguish main and remote hardware
profiles.

Common one-board checks:

- wrapper upload, boot and USB logging;
- identity/reset behavior;
- I2C target visibility/error handling;
- Bluetooth discovery/connection lifecycle.

Remote hardware checks additionally require:

- P1.04-P1.07 external LEDs;
- external GY-BMI160 presence on local D11/D12 I2C;
- local BME280 presence on the same D11/D12 I2C bus;
- valid accelerometer and gyroscope samples;
- pressure/temperature/humidity reads and sensor-snapshot freshness;
- static vertical, controlled tilt and return-to-vertical behavior;
- BMI160 missing/timeout/bad-data behavior;
- BME280 missing/timeout/bad-data behavior independently of BMI160;
- `3V3_OUT` sensor power and absence of a second RJ45 3.3 V source;
- warning/error latch persistence.

Main-enclosure checks must prove:

- boot/communication with no external four-LED assembly;
- boot/communication with no external GY-BMI160 or BME280;
- absence of those remote-only sensors/LEDs is not logged as a fatal main-node
  hardware error.

### E. Multi-node HIL

For two or more physical nRF boards, cover:

- basic two-node Channel Sounding pair;
- repeated pairwise ranging and quality/confidence;
- initiator/reflector rotation so every connected node initiates;
- distance-matrix updates;
- disconnect/reconnect;
- reboot of one node while others remain available;
- missing-node startup and recovery;
- warning/error indications on separate remote nodes.

### F. Full ESP32 + TCA topology HIL

Cover:

- ESP32 upstream I2C ownership;
- main nRF always visible;
- TCA9548A channel selection/deselection;
- one remote nRF per selected channel;
- each remote BME280 and GY-BMI160 on that node's private D11/D12 bus, not the
  ESP32/TCA bus;
- both local sensors powered from that XIAO's `3V3_OUT`; RJ45 pin 2 +3.3 V is
  RESERVED/NC and must not be paralleled with it;
- upstream reads the remote nRF snapshot rather than enumerating raw BME280 or
  BMI160 targets through TCA9548A;
- approved solution for possible identical nRF target addresses;
- ESP32/nRF contracts carrying identity, topology, orientation and ranging
  results without breaking seismic acquisition.

## 7. Multi-node matrix

The implementation/test harness must use discovered node count rather than a
hard-coded four-remote assumption:

| Topology | Physical nRF boards | Minimum purpose |
| --- | ---: | --- |
| main only | 1 | boot, no-remote topology, no remote-only IMU/LED requirement |
| 1 main + 1 remote | 2 | discovery, one GY-BMI160, first ranging pair and role rotation |
| 1 main + 2 remotes | 3 | multiple pairs, partial matrix, reconnect |
| 1 main + 3 remotes | 4 | channel mapping and multi-node scheduling |
| 1 main + 4 remotes | 5 | diploma reference topology and complete ranging cycle |
| supported N beyond four | 1 + N | capacity limits and graceful handling when hardware exists |

Five physical boards must never be required by ordinary software-only CI.

## 8. Regression contract

Future nRF changes must gate:

- ESP32-S3 configure/build and existing host/component tests;
- nRF wrapper production build;
- qualified nRF software/native tests;
- WebUI tests/build/typecheck as currently defined;
- protocol/schema contracts between ESP32 and nRF when introduced;
- architecture/pin/config consistency;
- one-production-firmware/no-central-remote-variant invariant.

Physical nRF HIL is a separate explicit tier. Missing hardware is `BLOCKED`,
not an implicit pass.

## 9. Evidence vocabulary

Keep evidence types separate:

- `COMPILES`: wrapper build/link completed;
- `HOST TESTED` / `NATIVE TESTED`: deterministic software tests passed;
- `TARGET TESTED`: image ran on one physical target with machine-observable
  evidence;
- `HIL TESTED`: a named physical scenario passed;
- `PHYSICALLY VERIFIED`: a named physical observation was confirmed.

A successful build or native simulation never proves GY-BMI160/BME280 wiring,
sensor power, GPIO LED behavior, physical Channel Sounding or real TCA
topology.

## 10. Open questions / implementation gates

1. **I2C identity/addressing:** choose and validate numeric discovery/main/remote
   address states, identity provisioning and collision recovery before wiring
   the production role-assignment/upstream command protocol. The platform
   adapter deliberately accepts caller-supplied 7-bit addresses and does not
   freeze this decision.
2. **Local sensor electrical contract:** verify the selected GY-BMI160
   breakout's `3V3`/`VIN`, `CS`/`SA0`, exposed primary SCL/SDA labels, pull-ups
   and unused auxiliary/interrupt pins; verify BME280 `SDO` address strap,
   pull-ups and module supply labels. Measure the combined `3V3_OUT` load,
   startup/transient margin and confirm RJ45 pin 2 is not tied to that rail.
3. **Production upstream protocol:** define command framing, snapshot framing,
   role-assignment/re-address transitions, one-hot TCA invariant, recovery and
   caller-owned numeric addresses. The Zephyr API boundary itself is no longer
   open: pinned Zephyr 4.2.1 uses standard buffer-mode `i2c_target_*` through
   the Nordic nrfx TWIS driver, and the project foundation wraps it without a
   custom HAL workaround.
4. **Snapshot/history contract:** define the upstream node-snapshot schema,
   sampling/freshness rules and any RAM history size before implementation.
5. **Channel Sounding maturity:** qualify initiator/reflector support,
   measurement format and scheduling limits on the selected package.
6. **Geometry contract:** define distance-matrix schema, confidence and
   missing/outlier policy before feeding localization/WebXR.
7. **Tilt UX:** approve filter, calibration, threshold/warning band,
   hysteresis/debounce, blink mapping, saturation and IMU-failure output.
8. **Combined logger LEDs:** confirm red-only priority versus simultaneous
   yellow+red.
9. **Automatic test selection:** qualify a truly hardware-free native/host
   execution route before adding executable nRF software suites to `Tests: All`.

## 11. Official references checked

- [Seeed XIAO nRF54L15 getting started and pin map](https://wiki.seeedstudio.com/xiao_nrf54l15_sense_getting_started/)
- [Seeed XIAO nRF54L15 PlatformIO configuration](https://wiki.seeedstudio.com/xiao_nrf54l15_with_platform_io/)
- [Seeed platform-seeedboards](https://github.com/Seeed-Studio/platform-seeedboards)
- [Bosch BMI160 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi160-ds000.pdf)
- [Bosch BME280 datasheet](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme280-ds002.pdf)
- [Zephyr `bosch,bmi160` binding](https://docs.zephyrproject.org/latest/build/dts/api/compatibles/bosch%2Cbmi160.html)
- [Zephyr `bosch,bme280` I2C binding](https://docs.zephyrproject.org/latest/build/dts/api/bindings/sensor/bosch%2Cbme280-i2c.html)
- [Zephyr BME280 Sensor API sample](https://docs.zephyrproject.org/latest/samples/sensor/bme280/README.html)
- [Zephyr Nordic TWIM binding](https://docs.zephyrproject.org/latest/build/dts/api/bindings/i2c/nordic%2Cnrf-twim.html)
- [Zephyr Nordic TWIS binding](https://docs.zephyrproject.org/latest/build/dts/api/bindings/i2c/nordic%2Cnrf-twis.html)
- [Zephyr 4.2.1 Nordic nrfx TWIS driver](https://github.com/zephyrproject-rtos/zephyr/blob/v4.2.1/drivers/i2c/i2c_nrfx_twis.c)
- [Zephyr 4.2.1 nRF54L15 I2C/TWIS loopback overlay](https://github.com/zephyrproject-rtos/zephyr/blob/v4.2.1/samples/drivers/i2c/rtio_loopback/boards/nrf54l15dk_nrf54l15_cpuapp.overlay)
- [Seeed XIAO nRF54L15 schematic](https://files.seeedstudio.com/wiki/XIAO_nRF54L15/Getting_Start/nRF54L15_Schematic.pdf)
- [Zephyr Channel Sounding sample](https://docs.zephyrproject.org/latest/samples/bluetooth/channel_sounding/README.html)
- [Nordic Bluetooth Channel Sounding overview](https://www.nordicsemi.com/Products/Wireless/Bluetooth-Low-Energy/Channel-Sounding)
- [PlatformIO `pio test`](https://docs.platformio.org/en/latest/core/userguide/cmd_test.html)
